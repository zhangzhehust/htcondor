/***************************Copyright-DO-NOT-REMOVE-THIS-LINE**
  *
  * Condor Software Copyright Notice
  * Copyright (C) 1990-2004, Condor Team, Computer Sciences Department,
  * University of Wisconsin-Madison, WI.
  *
  * This source code is covered by the Condor Public License, which can
  * be found in the accompanying LICENSE.TXT file, or online at
  * www.condorproject.org.
  *
  * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  * AND THE UNIVERSITY OF WISCONSIN-MADISON "AS IS" AND ANY EXPRESS OR
  * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
  * WARRANTIES OF MERCHANTABILITY, OF SATISFACTORY QUALITY, AND FITNESS
  * FOR A PARTICULAR PURPOSE OR USE ARE DISCLAIMED. THE COPYRIGHT
  * HOLDERS AND CONTRIBUTORS AND THE UNIVERSITY OF WISCONSIN-MADISON
  * MAKE NO MAKE NO REPRESENTATION THAT THE SOFTWARE, MODIFICATIONS,
  * ENHANCEMENTS OR DERIVATIVE WORKS THEREOF, WILL NOT INFRINGE ANY
  * PATENT, COPYRIGHT, TRADEMARK, TRADE SECRET OR OTHER PROPRIETARY
  * RIGHT.
  *
  ****************************Copyright-DO-NOT-REMOVE-THIS-LINE**/

#include "condor_classad.h"
#include "list.h"
#include "scheduler.h"


enum AllocStatus { A_NEW, A_RUNNING, A_DYING };

enum NegotiationResult { NR_MATCHED, NR_REJECTED, NR_END_NEGOTIATE, 
						 NR_LIMIT_REACHED, NR_ERROR };

class CAList : public List<ClassAd> {};
class MRecArray : public ExtArray<match_rec*> {};

class AllocationNode {
 public:
	AllocationNode( int cluster_id, int n_procs );
	~AllocationNode();

		// Methods
	void addResource( ClassAd* r, int proc );
	void setCapability( const char* new_capab );
	void display( void );

		// Data
	int status;
	char* capability;	// The capability for the first match in the cluster 
	int cluster;		// cluster id of the job(s) for this allocation
	int num_procs;			// How many procs are in the cluster
	ExtArray< ClassAd* >* jobs;		// Both arrays are indexed by proc
	ExtArray< MRecArray* >* matches;
	int num_resources;		// How many total resources have been allocated
};		


class ResTimeNode {
 public:
	ResTimeNode( time_t t );
	~ResTimeNode();
	
		/** Can we satisfy the given job with this ResTimeNode?  No
			matter what we return, num_matches is reset to the number
			of matches we found at this time, and the candidates list
			includes a pointer to each resource ad we matched with.
			@param jobAd The job to satisfy
			@param max_hosts How many resources does this job need?
			@param candidates List of pointers to ads that matched
			@return Was the job completely satisfied?
		*/
	bool satisfyJob( ClassAd* jobAd, int max_hosts,
					 CAList* candidates );

	void display( int level );

	time_t time;
	CAList* res_list;
	int num_matches;
};


class AvailTimeList : public List<ResTimeNode> {
 public:
	~AvailTimeList();
	void display( int debug_level );

		/// Returns if there are any resources available in our list.
	bool hasAvailResources( void );

		/** Add the resource described in the given match record into
			our list.  We find out when the resource will be
			available, and add the resource to our list in the
			appropriate ResTimeNode.  If no node exists for the given
			time, we create a new node.
			@param mrec The match record for the resource to add.  */
	void addResource( match_rec* mrec );

		/** Removes the resource classad from the given ResTimeNode in
			our list.  If that was the last resource in the
			ResTimeNode, we remove the node from our list, delete the
			object, and set the given rtn pointer to NULL.
			@param resource The resource to remove
			@param rtn The ResTimeNode to remove it from */
	void removeResource( ClassAd* resource, ResTimeNode* &rtn );
};


class DedicatedScheduler : public Service {
 public:
	DedicatedScheduler();
	~DedicatedScheduler();

		// Called at start-up to initialize this class.  This does the
		// work of finding which dedicated resources we control, and
		// starting the process of claiming them.
	int initialize( void );
	int shutdown_fast( void );
	int shutdown_graceful( void );
	int	reconfig( void );

		// Function to negotiate with the central manager for our MPI
		// jobs.  This is called by Scheduler::negotiate() if the
		// owner we get off the wire matches the "owner" string we're
		// using. 
	int negotiate( Stream* s, char* negotiator_name );

		// Called everytime we want to process the job queue and
		// schedule/spawn MPI jobs.
	int handleDedicatedJobs( void );

	void handleDedicatedJobTimer( int seconds );

		// Let the outside world know what we're doing... how exactly
		// this should work is still unclear
	void displaySchedule( void );

	void listDedicatedJobs( int debug_level );
	void listDedicatedResources( int debug_level, ClassAdList* resources );

		// Used for claiming/releasing startds we control
	bool contactStartd( ContactStartdArgs* args );
	bool requestClaim( ClassAd* r );
	int	startdContactSockHandler( Stream* sock );
	bool releaseClaim( match_rec* m_rec, bool use_tcp = true );
	bool deactivateClaim( match_rec* m_rec );
	void sendAlives( void );

		// Reaper for the MPI shadow
	int reaper( int pid, int status );

	int giveMatches( int cmd, Stream* stream );

		// These are public, since the Scheduler class needs to call
		// them from vacate_service and possibly other places, too.
	bool DelMrec( char* cap );
	bool DelMrec( match_rec* rec );

		/** Remove the given shadow record from any match records that
			point to it by setting the shadowRec field to NULL.
			@param shadow Pointer to the shadow record to remove
			@return true if any match records point to that shadow,
			        false if not.
		*/
	bool removeShadowRecFromMrec( shadow_rec* shadow );

	char* name( void ) { return ds_name; };
	char* owner( void ) { return ds_owner; };

		/** Publish a ClassAd to the collector that says we have
			resource requests we want to negotiate for.
		*/
	void publishRequestAd( void );

	void generateRequest( ClassAd* job );

		/** Clear out all existing resource requests.  Used at the
			begining of computeSchedule(), since, if there are still
			resource requests from the last schedule that we haven't
			negotiated for, we want to get rid of those and figure out 
			everything we need to request given the current state of
			things.
		*/
	void clearResourceRequests( void );
 
		// Set the correct value of ATTR_SCHEDULER in the queue for
		// the given job ad.
	bool setScheduler( ClassAd* job_ad );

		/// Clear out our data structure of idle dedicated jobs. 
	void clearDedicatedClusters( void );

		/// Add the given cluster to our set of idle dedicated jobs.
	void addDedicatedCluster( int cluster );

		/// Returns true if there are idle dedicated clusters.
	bool hasDedicatedClusters( void );

 private:

	/** Used to handle the negotiation protocol for a given
		resource request.  
		@param req ClassAd holding the resource request
		@param s The Stream to communicate with the CM
		@param negotiator_name The name of this negotiator
		@param reqs_matched How many requests have been matched already.
		@param max_reqs Total number of requests we're trying to meet. 
		@return An enum describing the results.
	*/
	NegotiationResult negotiateRequest( ClassAd* req, Stream* s, 
										char* negotiator_name, 
										int reqs_matched, 
										int max_reqs ); 

		// This gets a list of all dedicated resources we control.
		// This is called at the begining of each handleDedicatedJobs
		// cycle.
	bool getDedicatedResourceInfo( void );

		// This one should be seperated out, and most easy to change.
	bool computeSchedule( void );

		// This does the work of acting on a schedule, once that's
		// been decided.  
	bool spawnJobs( void );

		// Do through our list of pending resource requests, and
		// publish a ClassAd to the CM to ask for them.
	bool requestResources( void );

		// Print out all our pending resource requests.
	void displayResourceRequests( void );

	void sortResources( void );
	void clearResources( void );

	bool sortJobs( void );

        // Used to give matches to the mpi shadow when it asks for
		// them. 
    int giveMPIMatches( Service*, int cmd, Stream* stream );

		// Deactivate the claim on all resources used by this shadow
	void shutdownMpiJob( shadow_rec* srec );

		/** Update internal data structures to remove the allocation  
			associated with this shadow.
			@param srec Shadow record of the allocation to remove
		*/
	void removeAllocation( shadow_rec* srec );

	void addUnclaimedResource( ClassAd* resource );
	bool hasUnclaimedResources( void );
	void clearUnclaimedResources( void );

	void callHandleDedicatedJobs( void );

		/** Do a number of sanity-checks, like releasing resources
			we're holding onto and not using.
		*/
	void checkSanity( void );

		/** Check the given match record to make sure the claim hasn't
			been unused for too long.
			@param mrec The match record to check
			@return how many seconds this match has not been used
		*/
	int getUnusedTime( match_rec* mrec );

		/** Find the match record that corresponds to the given
			resource classad.  If the second argument is non-NULL, the
			value of ATTR_NAME from the given resource ad will be
			printed there.
			@param ad ClassAd for the resource you want to find
			@param buf An optional buffer to store ATTR_NAME
			@return pointer to the mrec if found, NULL if not
		*/
	match_rec* getMrec( ClassAd* ad, char* buf = NULL );

		/** Figure out if it's possible to ever schedule this job,
			given all of the dedicated resources we know about. 
			@param job ClassAd of the job to schedule
			@param max_hosts How many hosts the job is looking for
			@return true if possible, false if not
		*/
	bool isPossibleToSatisfy( ClassAd* job, int max_hosts );

	bool hasDedicatedShadow( void );

	void holdAllDedicatedJobs( void );

		// // // // // // 
		// Data members 
		// // // // // // 

		// Stuff for interacting w/ DaemonCore
	int		hdjt_tid;		// DC timer id for handleDedicatedJobTimer()
	int		sanity_tid;		// DC timer id for sanityCheck()
	int		rid;			// DC reaper id

		// data structures for managing dedicated jobs and resources. 
	ExtArray<int>*		idle_clusters;	// Idle cluster ids

	ClassAdList*		resources;		// All dedicated resources 

		// All resources, sorted by the time they'll next be available 
	AvailTimeList*			avail_time_list;	

		// All resources that might be dedicated to us that aren't
		// currently claimed.
	ResTimeNode*		unclaimed_resources;

        // hashed on cluster, all our allocations
    HashTable <int, AllocationNode*>* allocations;

		// hashed on resource name, each claim we have
	HashTable <HashKey, match_rec*>* all_matches;

		// hashed on capability, each claim we have.  only store
		// pointers in here into the real match records we store in
		// all_matches.  This is needed for some functions that only
		// know the capability (like DelMrec(), since vacate_service()
		// is only given a capability to identify the lost claim).
	HashTable <HashKey, match_rec*>* all_matches_by_cap;

		// Queue for resource requests we need to negotiate for. 
	Queue<ClassAd*>* resource_requests;

	int		num_matches;	// Total number of matches in all_matches 

    static const int MPIShadowSockTimeout;

	ClassAd		dummy_job;		// Dummy ad used for claiming startds 

	char* ds_name;		// Name of this dedicated scheduler.  Also
		                // used for ATTR_SCHEDULER.
	char* ds_owner;		// "Owner" to identify this dedicated scheduler 

	int unused_timeout;	// How many seconds are we willing to hold
		// onto a resource without using it before we release it? 

	Shadow* shadow_obj;
};


// ////////////////////////////////////////////////////////////
//   Utility functions
// ////////////////////////////////////////////////////////////

// Find when a given resource will next be available
time_t findAvailTime( match_rec* mrec );

// Comparison function for sorting job cluster ids by QDate
int clusterSortByDate( const void* ptr1, const void* ptr2 );

// Print out
void displayResource( ClassAd* ad, char* str, int debug_level );
void displayRequest( ClassAd* ad, char* str, int debug_level );

// Clear out all the fields in the match record that have anything to
// do with the mrec being allocated to a certain MPI job.
void deallocMatchRec( match_rec* mrec );

