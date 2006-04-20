/*
 * iterated_hash.h -- nsec3 hash calculation.
 *
 * Copyright (c) 2001-2006, NLnet Labs. All rights reserved.
 *
 * See LICENSE for the license.
 *
 * With thanks to Ben Laurie.
 */
#ifndef ITERATED_HASH_H
#define ITERATED_HASH_H

#include <config.h>
#ifdef NSEC3
#include <openssl/sha.h>

#define HASHED_NAME_LENGTH     36

struct domain;
struct dname;
struct region;
struct zone;
struct namedb;

int iterated_hash(unsigned char out[SHA_DIGEST_LENGTH],
	const unsigned char *salt,int saltlength,
	const unsigned char *in,int inlength,int iterations);
const struct dname *nsec3_hash_dname(struct region *region, 
	struct zone *zone, const struct dname *dname);

/* calculate prehash information for the given zone,
  or all zones if zone == NULL */
void prehash(struct namedb* db, struct zone* zone);

#endif /* NSEC3 */
#endif /* ITERATED_HASH_H */
