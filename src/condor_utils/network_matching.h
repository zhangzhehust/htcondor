
#ifndef __NETWORK_MATCHING_H_
#define __NETWORK_MATCHING_H_

#include <classad/classad.h>

/*
 * Given a job ad, a machine ad, and a schedd ad, determine whether we have a network match.
 *
 * The TransferQueueUserExpr is taken from the schedd ad and evaluated in the scope of the match
 * ad of the job and machine.  This allows us to determine which transfer queue will be used
 * for the I/O related to this job.
 *
 * Next, the queued volume for the specified queue is looked up in the schedd ad and the bandwidth
 * between the machine and schedd is estimated.  If the backlog of transfers is too high, this
 * will return false.
 *
 * If any of the required attributes are missing, this will return true.
 */
bool
IsANetworkMatch(classad::ClassAd &jobAd, classad::ClassAd &machineAd, const classad::ClassAd &scheddAd);

/*
 * This function takes link speed information from the system network devices (if available) and
 * adds the informatino to the given ClassAd.
 *
 * The purpose is to provide a reasonable default for network bandwidth limits.  More realistic
 * estimates can be provided by external services.
 *
 */
bool
PopulateDefaultNetworkAd(classad::ClassAd &ad);

#endif /*__NETWORK_MATCHING_H_*/

