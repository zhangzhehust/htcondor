
#include "condor_common.h"
#include "network_matching.h"
#include <classad/matchClassad.h>

#include "condor_attributes.h"
#include "condor_debug.h"
#include "condor_config.h"

/*
 * A slight twist on classad's MatchClassAd.
 * This is meant to be a RAII-style class for matchmaking.
 * Once the TemporaryMatchAd is destroyed, the left and right
 * ads are put back into their original state.
 */
class TemporaryMatchAd : public classad::MatchClassAd
{
public:
	TemporaryMatchAd(ClassAd &lad, ClassAd &rad) : MatchClassAd(&lad, &rad)
	{
		if ( !compat_classad::ClassAd::m_strictEvaluation ) {
			lad.alternateScope = &rad;
			rad.alternateScope = &lad;
		}
	}

	virtual ~TemporaryMatchAd()
	{
		classad::ClassAd *ad = RemoveLeftAd();
		ad->alternateScope = NULL;
		ad = RemoveRightAd();
		ad->alternateScope = NULL;
	}
};

/*
 * A RAII-style object for managing the scope of a classad ExprTree.
 */
class ScopeGuard
{
public:
	ScopeGuard(classad::ExprTree &expr, const classad::ClassAd *scope_ptr)
	  : m_orig(expr.GetParentScope()), m_expr(expr), m_new(scope_ptr)
	{
		if (m_new) m_expr.SetParentScope(scope_ptr);
	}
	~ScopeGuard()
	{
		if (m_new) m_expr.SetParentScope(m_orig);
	}

private:
	const classad::ClassAd *m_orig;
	classad::ExprTree &m_expr;
	const classad::ClassAd *m_new;
};

/*
 * Iterate through the list of ClassAds in the schedd ad's attribute "NetworkAds".
 * For each ad, see if its requirements match the given machine ad.
 * If they do, then use the value of BandwidthDownMbps and BandwidthUpMbps in the
 * network ad as the bandwidth estiamte.
 *
 * If there are multiple matches, use the smallest value for bandwidth up/down.
 *
 * After evaluating the network ads in the schedd, evaluate the network ads
 * given in the machine using the same algorithm.
 *
 */
bool
EstimateNetworkBandwidth(const classad::ClassAd &scheddAd, const classad::ClassAd &machineAd, long &bandwidth_down_mbps, long &bandwidth_up_mbps)
{
	// TODO: In the future, use Network ads to estimate
	classad::Value val;
	classad_shared_ptr<classad::ExprList> list;
	bandwidth_down_mbps = -1;
	bandwidth_up_mbps = -1;
	long tmp_bandwidth_down_mbps, tmp_bandwidth_up_mbps;
	if (scheddAd.EvaluateAttr("NetworkAds", val) && val.IsSListValue(list)) {
		for (classad::ExprList::const_iterator it = list->begin(); it != list->end(); it++) {
			if (!(*it)->isClassad()) { continue; }
			TemporaryMatchAd match_ad(*static_cast<ClassAd*>(*it), const_cast<classad::ClassAd&>(machineAd));
			if (!match_ad.leftMatchesRight()) {continue;}
			classad::ClassAd *network_ad = match_ad.GetLeftContext();
			if (network_ad->EvaluateAttrNumber("BandwidthDownMbps", tmp_bandwidth_down_mbps) )
			{
				if ( (bandwidth_down_mbps == -1) || (bandwidth_down_mbps > tmp_bandwidth_down_mbps) ) {
					bandwidth_down_mbps = tmp_bandwidth_down_mbps;
				}
			}
			if (network_ad->EvaluateAttrNumber("BandwidthUpMbps", tmp_bandwidth_up_mbps) )
			{
				if ( (bandwidth_up_mbps == -1) || (bandwidth_up_mbps > tmp_bandwidth_up_mbps) ) {
					bandwidth_up_mbps = tmp_bandwidth_up_mbps;
				}
			}
		}
	}
	if (machineAd.EvaluateAttr("NetworkAds", val) && val.IsSListValue(list)) {
		for (classad::ExprList::const_iterator it = list->begin(); it != list->end(); it++) {
			if (!(*it)->isClassad()) { continue; }
			TemporaryMatchAd match_ad(*static_cast<ClassAd*>(*it), const_cast<classad::ClassAd&>(scheddAd));
			if (!match_ad.leftMatchesRight()) {continue;}
			classad::ClassAd *network_ad = match_ad.GetLeftContext();
			if (network_ad->EvaluateAttrNumber("BandwidthDownMbps", tmp_bandwidth_down_mbps) )
			{
				if ( (bandwidth_down_mbps == -1) || (bandwidth_down_mbps > tmp_bandwidth_down_mbps) ) {
					bandwidth_down_mbps = tmp_bandwidth_down_mbps;
				}
			}
			if (network_ad->EvaluateAttrNumber("BandwidthUpMbps", tmp_bandwidth_up_mbps) )
			{
				if ( (bandwidth_up_mbps == -1) || (bandwidth_up_mbps > tmp_bandwidth_up_mbps) ) {
					bandwidth_up_mbps = tmp_bandwidth_up_mbps;
				}
			}
		}
	}
	return (bandwidth_up_mbps >= 0) && (bandwidth_down_mbps >= 0);
}

bool
IsANetworkMatch(classad::ClassAd &jobAd, classad::ClassAd &machineAd, const classad::ClassAd &scheddAd)
{
	TemporaryMatchAd match_ad(jobAd, machineAd);

	classad::ExprTree *expr = scheddAd.Lookup(ATTR_TRANSFER_QUEUE_USER_EXPR);
	if (!expr) {
		dprintf(D_FULLDEBUG, "Unable to determine the transfer queue expression from the schedd ad.\n");
		return true;
	}
        classad::ClassAd *ad_for_eval = match_ad.GetLeftContext();
	ScopeGuard scope(*expr, ad_for_eval);
	std::string transfer_queue;
	classad::Value v;
	if (!expr->Evaluate(v) || !v.IsStringValue(transfer_queue)) {
		dprintf(D_FULLDEBUG, "Unable to evaluate the transfer queue expression to a string.\n");
		return true;
	}

	long mb_for_download, mb_for_upload;
	if (!scheddAd.EvaluateAttrNumber(transfer_queue + "FileTransferMBWaitingToDownload", mb_for_download))
	{
		dprintf(D_FULLDEBUG, "Unable to determine MB waiting to download for transfer queue %s.\n", transfer_queue.c_str());
		return true;
	}
	if (!scheddAd.EvaluateAttrNumber(transfer_queue + "FileTransferMBWaitingToUpload", mb_for_upload))
	{
		dprintf(D_FULLDEBUG, "Unable to determine MB waiting to upload for transfer queue %s.\n", transfer_queue.c_str());
		return true;
	}
	long bandwidth_down_mbps, bandwidth_up_mbps;
	if (!EstimateNetworkBandwidth(scheddAd, machineAd, bandwidth_down_mbps, bandwidth_up_mbps))
	{
		bandwidth_down_mbps = param_integer("NEGOTIATOR_ESTIMATED_BANDWIDTH_DOWN_MBPS", 1000);
		bandwidth_up_mbps = param_integer("NEGOTIATOR_ESTIMATED_BANDWIDTH_UP_MBPS", 1000);
		dprintf(D_FULLDEBUG, "Unable to estimate network bandwidth; using defaults of %ld mbps down / %ld mbps up.\n", bandwidth_down_mbps, bandwidth_up_mbps);
	}
	long max_wait = param_integer("NEGOTIATOR_MAX_ESTIMATED_NETWORK_WAIT", 600);
	long est_download_wait = mb_for_download * 8 / bandwidth_down_mbps;
	long est_upload_wait = mb_for_upload * 8 / bandwidth_up_mbps;
	if (max_wait < est_download_wait) {
		dprintf(D_FULLDEBUG, "Rejecting match due to insufficient download bandwidth.  Estimated wait is %ld of max %ld.\n", est_download_wait, max_wait);
		return false;
	}
	if (max_wait < est_upload_wait) {
		dprintf(D_FULLDEBUG, "Rejecting match due to insufficient upload bandwidth.  Estimated wait is %ld of max %ld.\n", est_upload_wait, max_wait);
		return false;
	}
	dprintf(D_FULLDEBUG, "Match has sufficient bandwidth.  Estimated bandwidth is %ld Mbps down / %ld Mbps up.  There are %ld and %ld MB in queue %s, for an estimated wait time of %lds for downloads and %lds for uploads, respectively.\n", bandwidth_down_mbps, bandwidth_up_mbps, mb_for_download, mb_for_upload, transfer_queue.c_str(), est_download_wait, est_upload_wait);
	return true;
}

