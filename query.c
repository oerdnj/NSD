/*
 * $Id: query.c,v 1.66 2002/05/06 19:21:30 alexis Exp $
 *
 * query.c -- nsd(8) the resolver.
 *
 * Alexis Yushin, <alexis@nlnetlabs.nl>
 *
 * Copyright (c) 2001, NLnet Labs. All rights reserved.
 *
 * This software is an open source.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * Neither the name of the NLNET LABS nor the names of its contributors may
 * be used to endorse or promote products derived from this software without
 * specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 */
#include "nsd.h"


/*
 * Stript the packet and set format error code.
 *
 */
void
query_formerr(q)
	struct query *q;
{
	RCODE_SET(q, RCODE_FORMAT);

	/* Truncate the question as well... */
	QDCOUNT(q) = ANCOUNT(q) = NSCOUNT(q) = ARCOUNT(q) = 0;
	q->iobufptr = q->iobuf + QHEADERSZ;
}

void
query_init(q)
	struct query *q;
{
	q->addrlen = sizeof(q->addr);
	q->iobufsz = QIOBUFSZ;
	q->iobufptr = q->iobuf;
	q->maxlen = 512;	/* XXX Should not be here */
	q->edns = 0;
	q->tcp = 0;
}

void
query_addanswer(q, dname, a, truncate)
	struct query *q;
	u_char *dname;
	struct answer *a;
	int truncate;
{
	u_char *qptr;
	u_int16_t pointer;
	int  i, j;

	/* Check that the answer fits into our query buffer... */
	if(ANSWER_DATALEN(a) > (q->iobufptr - q->iobuf + q->iobufsz)) {
		syslog(LOG_ERR, "the answer in the database is larger then the query buffer");
		RCODE_SET(q, RCODE_SERVFAIL);
		return;
	}

	/* Copy the counters */
	ANCOUNT(q) = ANSWER_ANCOUNT(a);
	NSCOUNT(q) = ANSWER_NSCOUNT(a);
	ARCOUNT(q) = ANSWER_ARCOUNT(a);

	/* Then copy the data */
	bcopy(ANSWER_DATA_PTR(a), q->iobufptr, ANSWER_DATALEN(a));

	/* Walk the pointers */
	for(j = 0; j < ANSWER_PTRSLEN(a); j++) {
		qptr = q->iobufptr + ANSWER_PTRS(a, j);
		bcopy(qptr, &pointer, 2);
		switch((pointer & 0xf000)) {
		case 0xc000:			/* This pointer is relative to the name in the query.... */
			/* XXX Check if dname is within packet */
			pointer = htons(0xc000 | (dname - q->iobuf + (pointer & 0x0fff)));/* dname - q->iobuf */
			break;
		case 0xd000:			/* This is the wildcard */
			pointer = htons(0xc00c);
			break;
		default:
			/* This pointer is relative to the answer that we have in the database... */
			pointer = htons(0xc000 | (u_int16_t)(pointer + q->iobufptr - q->iobuf));
		}
		bcopy(&pointer, qptr, 2);
	}

	/* If we dont need truncation, return... */
	if(!truncate) {
		q->iobufptr += ANSWER_DATALEN(a);
		return;
	}

	/* Truncate if necessary */
	if(q->maxlen < (q->iobufptr - q->iobuf + ANSWER_DATALEN(a))) {

		/* Start with the additional section, record by record... */
		for(i = ntohs(ANSWER_ARCOUNT(a)) - 1, j = ANSWER_RRSLEN(a) - 1; i > 0 && j > 0; j--, i--) {
			if(q->maxlen >= (q->iobufptr - q->iobuf + ANSWER_RRS(a, j - 1))) {
				/* Make sure we remove the entire RRsets... */
				while(ANSWER_RRS_COLOR(a, j - 1) == ANSWER_RRS_COLOR(a, j - 2)) {
					j--; i--;
				}
				ARCOUNT(q) = htons(i-1);
				q->iobufptr += ANSWER_RRS(a, j - 1);
				return;
			}
		}

		ARCOUNT(q) = htons(0);
		TC_SET(q);

		if(q->maxlen >= (q->iobufptr - q->iobuf + ANSWER_RRS(a, j - ntohs(a->nscount) - 1))) {
			/* Truncate the athority section */
			NSCOUNT(q) = htons(0);
			q->iobufptr += ANSWER_RRS(a, j - ntohs(a->nscount) - 1);
			return;
		}

		/* Send empty message */
		NSCOUNT(q) = 0;
		ANCOUNT(q) = 0;

		return;
	} else {
		q->iobufptr += ANSWER_DATALEN(a);
	}
}

int
query_axfr(q, db, qname, zname, depth)
	struct query *q;
	struct namedb *db;
	u_char *qname;
	u_char *zname;
	int depth;
{
	static rbnode_t *node;
	static struct domain *d;
	static struct answer *a;
	static u_char *dname;
	u_char *skipzone = NULL;
	u_char *qptr;
	u_char iname[MAXDOMAINLEN+1];

	/* Per AXFR... */
	static u_char *zone, *qnameptr;
	static struct answer *soa;

	/* Is it new AXFR? */
	if(qname) {
		/* New AXFR... */
		zone = zname;
		qnameptr = qname;

		/* Do we have the SOA? */
		if(NAMEDB_TSTBITMASK(db, NAMEDB_DATAMASK, depth)) {
			dnameinvert(zone, iname);

			if((node = heap_locate(db->iheap, iname)) == NULL) {
				/* No SOA no transfer */
				RCODE_SET(q, RCODE_REFUSE);
				return 0;
			}

			dname = node->data;
			d = node->data + (((u_int32_t)*((char *)node->data) + 1 + 3) & 0xfffffffc);

			/* XXX We rely here that SOA will always be the first answer */
			if((a = namedb_answer(d, htons(TYPE_SOA))) == NULL) {
				/* No SOA no transfer */
				RCODE_SET(q, RCODE_REFUSE);
				return 0;
			}
			soa = a;

			/* We'd rather have ANY than SOA to improve performance */
			if((a = namedb_answer(d, htons(TYPE_ANY))) == NULL) {
				a = soa;
			}

			qptr = q->iobufptr;

			query_addanswer(q, qname, a, 0);

			/* Truncate */
			NSCOUNT(q) = 0;
			ARCOUNT(q) = 0;
			q->iobufptr = qptr + ANSWER_RRS(a, ntohs(ANCOUNT(q)));

			return 1;
		}
	}

	/* We've done everything already, let the server know... */
	if(zone == NULL) {
		return 0;	/* Done. */
	}

	/* Let get next answer */
	a = NULL;

	/* Get next answer */
	while(a == NULL) {
		node = rbtree_next(node);

		/* End of the tree? */
		if(node == rbtree_last()) {
			a = soa;
			dname = zone;
			zone = NULL;
			break;
		} else {
			/* Get the name... */
			dname = node->data;
			d = node->data + (((u_int32_t)*((char *)node->data) + 1 + 3) & 0xfffffffc);

			/* Are we skipping an embedded zone? */
			if(skipzone != NULL && *skipzone <= *dname &&
				bcmp(skipzone + 1, dname + (*dname - *skipzone) + 1, *skipzone) == 0) {
				continue;
			} else {
				skipzone = NULL;
			}

			/* Outside the zone? */
			if((*zone > *dname) || (bcmp(zone + 1, dname + (*dname - *zone) + 1, *zone) != 0)) {
				a = soa;
				dname = zone;
				zone = NULL;
				break;
			}
		}

		if(DOMAIN_FLAGS(d) & NAMEDB_DELEGATION) {
			a = namedb_answer(d, htons(TYPE_NS));
		} else {
			/* Do we have an embedded zone? */
			if((a = namedb_answer(d, htons(TYPE_SOA))) != NULL) {
				skipzone = dname;
				a = NULL;
			} else {
				a = namedb_answer(d, htons(TYPE_ANY));
			}
		}
	}

	/* Existing AXFR, strip the question section off... */
	/* Very interesting math... Can you figure it? */
	q->iobufptr = q->iobuf + QHEADERSZ + *dname - 2;
	QDCOUNT(q) = ANCOUNT(q) = NSCOUNT(q) = ARCOUNT(q) = 0;

	qptr = q->iobufptr;

	query_addanswer(q, q->iobuf + QHEADERSZ, a, 0);
	bcopy(dname + 1, q->iobuf + QHEADERSZ, *dname);

	/* Truncate */
	NSCOUNT(q) = 0;
	ARCOUNT(q) = 0;
	q->iobufptr = qptr + ANSWER_RRS(a, ntohs(ANCOUNT(q)));

	/* More data... */
	return 1;
}

int
query_process(q, db)
	struct query *q;
	struct namedb *db;
{
	u_char qstar[2] = "\001*";
	u_char qnamebuf[MAXDOMAINLEN + 3];

	/* The query... */
	u_char	*qname, *qnamelow;
	u_char qnamelen;
	u_int16_t qtype;
	u_int16_t qclass;
	u_char *qptr;
	int qdepth, i;

	/* OPT record type... */
	u_int16_t opt_type, opt_class, opt_rdlen;

	struct domain *d;
	struct answer *a;
	int match;

	/* Sanity checks */
	if(QR(q)) return -1;	/* Not a query? Drop it on the floor. */

	/* Do we serve this type of query */
	if(OPCODE(q) != OPCODE_QUERY) {
		/* Setup the header... */
		QR_SET(q);		/* This is an answer */

		RCODE_SET(q, RCODE_REFUSE);

		/* Truncate the question as well... */
		QDCOUNT(q) = ANCOUNT(q) = NSCOUNT(q) = ARCOUNT(q) = 0;
		q->iobufptr = q->iobuf + QHEADERSZ;

		return 0;
	}

	/* Setup the header... */
	*(u_int16_t *)(q->iobuf + 2) = 0;
	QR_SET(q);		/* This is an answer */


	/* Dont bother to answer more than one question at once... */
	if(ntohs(QDCOUNT(q)) != 1) {
		query_formerr(q);
		return 0;
	}

	/* Lets parse the qname and convert it to lower case */
	qdepth = 0;
	qnamelow = qnamebuf + 3;
	qname = qptr = q->iobuf + QHEADERSZ;
	while(*qptr) {
		/*  If we are out of buffer limits or we have a pointer in question dname or the domain name is longer than MAXDOMAINLEN ... */
		if((qptr + *qptr > q->iobufptr) || (*qptr & 0xc0) ||
			((qptr - q->iobuf + *qptr) > MAXDOMAINLEN)) {

			query_formerr(q);
			return 0;
		}
		qdepth++;
		*qnamelow++ = *qptr;
		for(i = *qptr++; i; i--) {
			*qnamelow++ = NAMEDB_NORMALIZE(*qptr++);
		}
	}
	*qnamelow++ = *qptr++;
	qnamelow = qnamebuf + 3;

	/* Make sure name is not too long or we have stripped packet... */
	if((qnamelen = qptr - (q->iobuf + QHEADERSZ)) > MAXDOMAINLEN || TC(q) ||
		(qptr + 4 > q->iobufptr)) {
		query_formerr(q);
		return 0;
	}

	bcopy(qptr, &qtype, 2); qptr += 2;
	bcopy(qptr, &qclass, 2); qptr += 2;

	/* Dont allow any records in the answer or authority section... */
	if(ANCOUNT(q) != 0 || NSCOUNT(q) != 0) {
		query_formerr(q);
		return 0;
	}

	/* Do we have an OPT record? */
	if(ARCOUNT(q) > 0) {
		/* Only one opt is allowed... */
		if(ntohs(ARCOUNT(q)) != 1) {
			query_formerr(q);
			return 0;
		}

		/* Must have root owner name... */
		if(*qptr != 0) {
			query_formerr(q);
			return 0;
		}

		/* Must be of the type OPT... */
		bcopy(qptr + 1, &opt_type, 2);
		if(ntohs(opt_type) != TYPE_OPT) {
			query_formerr(q);
			return 0;
		}

		/* Ok, this is EDNS(0) packet... */
		q->edns = 1;

		/* Get the UDP size... */
		bcopy(qptr + 3, &opt_class, 2);
		opt_class = ntohs(opt_class);

		/* Check the version... */
		if(*(qptr + 6) != 0) {
			RCODE_SET(q, RCODE_IMPL);
			return 0;
		}

		/* Make sure there are no other options... */
		bcopy(qptr + 9, &opt_rdlen, 2);
		if(opt_rdlen != 0) {
			RCODE_SET(q, RCODE_IMPL);
			return 0;
		}

		/* Only care about UDP size larger than normal... */
		if(opt_class > 512) {
			/* XXX Configuration parameter to limit the size needs to be here... */
			if(opt_class < q->iobufsz) {
				q->maxlen = opt_class;
			} else {
				q->maxlen = q->iobufsz;
			}
		}

#ifdef	STRICT_MESSAGE_PARSE
		/* Trailing garbage? */
		if((qptr + OPT_LEN) != q->iobufptr) {
			query_formerr(q);
			return 0;
		}
#endif

		/* Strip the OPT resource record off... */
		q->iobufptr = qptr;
		ARCOUNT(q) = 0;
	}

	/* Do we have any trailing garbage? */
	if(qptr != q->iobufptr) {
#ifdef	STRICT_MESSAGE_PARSE
		/* If we're strict.... */
		query_formerr(q);
		return 0;
#else
		/* Otherwise, strip it... */
		q->iobufptr = qptr;
#endif
	}

	/* Unsupported class */
	if((ntohs(qclass) != CLASS_IN) && (ntohs(qclass) != CLASS_ANY)) {
		RCODE_SET(q, RCODE_REFUSE);
		return 0;
	}

	/* Prepare the name... */
	*(qnamelow - 1) = qnamelen;

	/* Is it AXFR? */
	switch(ntohs(qtype)) {
	case TYPE_AXFR:
			if(q->tcp)
				return query_axfr(q, db, qname, qnamelow - 1, qdepth);
	case TYPE_IXFR:
			RCODE_SET(q, RCODE_REFUSE);
			return 0;
			break;
	}

	/* BEWARE: THE RESOLVING ALGORITHM STARTS HERE */

	/* Do we have complete name? */
	if(NAMEDB_TSTBITMASK(db, NAMEDB_DATAMASK, qdepth) && ((d = namedb_lookup(db, qnamelow - 1)) != NULL)) {
		/* Is this a delegation point? */
		if(DOMAIN_FLAGS(d) & NAMEDB_DELEGATION) {
			if((a = namedb_answer(d, htons(TYPE_NS))) == NULL) {
				RCODE_SET(q, RCODE_SERVFAIL);
				return 0;
			}
			AA_CLR(q);
			query_addanswer(q, qname, a, 1);
			return 0;
		} else {
			if((a = namedb_answer(d, qtype)) != NULL) {
				if(ntohs(qclass) != CLASS_ANY) {
					query_addanswer(q, qname, a, 1);
					AA_SET(q);
					return 0;
				}
				/* Class ANY */
				AA_CLR(q);

				/* Setup truncation */
				qptr = q->iobufptr;

				query_addanswer(q, qname, a, 0);

				/* Truncate */
				NSCOUNT(q) = 0;
				ARCOUNT(q) = 0;
				q->iobufptr = qptr + ANSWER_RRS(a, ntohs(ANCOUNT(q)));

				return 0;
			} else {
				/* Do we have SOA record in this domain? */
				if((a = namedb_answer(d, htons(TYPE_SOA))) != NULL) {

					if(ntohs(qclass) != CLASS_ANY) {
						AA_SET(q);

						/* Setup truncation */
						qptr = q->iobufptr;

						query_addanswer(q, qname, a, 0);

						/* Truncate */
						ANCOUNT(q) = 0;
						NSCOUNT(q) = htons(1);
						ARCOUNT(q) = 0;
						if(ANSWER_RRSLEN(a) > 1)
							q->iobufptr = qptr + ANSWER_RRS(a, 1);

					} else {
						AA_CLR(q);
					}

					return 0;
				}

				/* We have a partial match */
				match = 1;
			}
		}
	} else {
		/* Set this if we find SOA later */
		RCODE_SET(q, RCODE_NXDOMAIN);
		match = 0;
	}

	/* Start matching down label by label */
	do {
		/* Strip leftmost label */
		qnamelen -= (*qname + 1);
		qname += (*qname + 1);
		qnamelow += (*qnamelow + 1);

		qdepth--;
		/* Only look for wildcards if we did not have any match before */
		if(match == 0 && NAMEDB_TSTBITMASK(db, NAMEDB_STARMASK, qdepth + 1)) {
			/* Prepend star */
			bcopy(qstar, qnamelow - 2, 2);

			/* Lookup star */
			*(qnamelow - 3) = qnamelen + 2;
			if((d = namedb_lookup(db, qnamelow - 3)) != NULL) {
				/* We found a domain... */
				RCODE_SET(q, RCODE_OK);

				if((a = namedb_answer(d, qtype)) != NULL) {
					if(ntohs(qclass) != CLASS_ANY) {
						AA_SET(q);
						query_addanswer(q, qname - 2, a, 1);
						return 0;
					}

					/* Class ANY */
					AA_CLR(q);

					/* Setup truncation */
					qptr = q->iobufptr;

					query_addanswer(q, qname, a, 0);

					/* Truncate */
					NSCOUNT(q) = 0;
					ARCOUNT(q) = 0;
					q->iobufptr = qptr + ANSWER_RRS(a, ntohs(ANCOUNT(q)));

					return 0;
				}
			}
		}

		/* Do we have a SOA or zone cut? */
		*(qnamelow - 1) = qnamelen;
		if(NAMEDB_TSTBITMASK(db, NAMEDB_AUTHMASK, qdepth) && ((d = namedb_lookup(db, qnamelow - 1)) != NULL)) {
			if(DOMAIN_FLAGS(d) & NAMEDB_DELEGATION) {
				if((a = namedb_answer(d, htons(TYPE_NS))) == NULL) {
					RCODE_SET(q, RCODE_SERVFAIL);
					return 0;
				}
				RCODE_SET(q, RCODE_OK);
				AA_CLR(q);
				query_addanswer(q, qname, a, 1);
				return 0;
			} else {
				if((a = namedb_answer(d, htons(TYPE_SOA)))) {

					if(ntohs(qclass) != CLASS_ANY) {
						/* Setup truncation */
						qptr = q->iobufptr;

						AA_SET(q);

						query_addanswer(q, qname, a, 0);

						/* Truncate */
						ANCOUNT(q) = 0;
						NSCOUNT(q) = htons(1);
						ARCOUNT(q) = 0;
						if(ANSWER_RRSLEN(a) > 1)
							q->iobufptr = qptr + ANSWER_RRS(a, 1);

					} else {
						AA_CLR(q);
					}

					return 0;
				}
			}
			/* We found some data, so dont try to match the wildcards anymore... */
			match = 1;
		}

	} while(*qname);

	RCODE_SET(q, RCODE_SERVFAIL);
	return 0;
}
