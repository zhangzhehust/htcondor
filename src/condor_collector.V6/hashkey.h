#ifndef __COLLHASH_H__
#define __COLLHASH_H__

#include "condor_common.h"
#include <iostream.h>
#include <stdio.h>
#include <string.h>
#ifndef WIN32
#include <netinet/in.h>
#endif
#include "condor_classad.h"

#include "HashTable.h"

// this is the tuple that we will be hashing on
class HashKey
{
    public:

    char name    [64];
    char ip_addr [16];

	void   sprint (char *);
	friend ostream& operator<< (ostream &out, const HashKey &); 
    friend bool operator== (const HashKey &, const HashKey &);
};

// the hash functions
int hashFunction (const HashKey &, int);
int hashOnName   (const HashKey &, int);

// type for the hash tables ...
typedef HashTable <HashKey, ClassAd *> CollectorHashTable;

// functions to make the hashkeys
bool makeStartdAdHashKey (HashKey &, ClassAd *, sockaddr_in *);
bool makeScheddAdHashKey (HashKey &, ClassAd *, sockaddr_in *);
bool makeMasterAdHashKey (HashKey &, ClassAd *, sockaddr_in *);
bool makeCkptSrvrAdHashKey (HashKey &, ClassAd *, sockaddr_in *);

#endif __COLLHASH_H__





