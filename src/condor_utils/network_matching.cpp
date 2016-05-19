
#include "condor_common.h"
#include "network_matching.h"
#include <classad/matchClassad.h>

#include "condor_attributes.h"
#include "condor_debug.h"
#include "condor_config.h"
#include "../condor_sysapi/sysapi.h"

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
	MyString job_classad_str;
	MyString machine_classad_str;
	MyString schedd_classad_str;
	sPrintAd(job_classad_str, jobAd);
	sPrintAd(machine_classad_str, machineAd);
	sPrintAd(schedd_classad_str, scheddAd);
	dprintf(D_FULLDEBUG, "The job classad is: %s\n", job_classad_str.Value());
	dprintf(D_FULLDEBUG, "The machine classad is: %s\n", machine_classad_str.Value());
	dprintf(D_FULLDEBUG, "The Schedd classad is: %s\n", schedd_classad_str.Value());
	TemporaryMatchAd match_ad(jobAd, machineAd);

	classad::ExprTree *expr = scheddAd.Lookup(ATTR_TRANSFER_QUEUE_USER_EXPR);
	if (!expr) {
		dprintf(D_FULLDEBUG, "Unable to determine the transfer queue expression from the schedd ad.\n");
		return true;
	}
	classad::ClassAd *ad_for_eval = match_ad.GetLeftContext();
	MyString ad_eval_str;
	sPrintAd(ad_eval_str, *ad_for_eval);
	dprintf(D_FULLDEBUG, "Ad for evaluation is: %s\n", ad_eval_str.Value());
	jobAd.ChainToAd(&machineAd);
	std::string cms_site;
	if (!jobAd.EvaluateAttrString("Glidein_CMSSite", cms_site)) {
		dprintf(D_FULLDEBUG, "Unable to evaluate Glidein_CMSSite to a string.\n");
	}
	else {
		dprintf(D_FULLDEBUG, "Remote execution CMS site is: %s\n", cms_site.c_str());
	}
	ScopeGuard scope(*expr, &jobAd);
	std::string transfer_queue;
	classad::Value v;
	if (!expr->Evaluate(v) || !v.IsStringValue(transfer_queue)) {
		dprintf(D_FULLDEBUG, "Unable to evaluate the transfer queue expression to a string.\n");
		jobAd.Unchain();
		return true;
	}
	jobAd.Unchain();

	long mb_for_download = 0; 
	long mb_for_upload = 0;
	if (!scheddAd.EvaluateAttrNumber(transfer_queue + "_FileTransferMBWaitingToDownload", mb_for_download))
	{
		dprintf(D_FULLDEBUG, "Unable to determine MB waiting to download for transfer queue %s.\n", transfer_queue.c_str());
		//return true;
	}
	if (!scheddAd.EvaluateAttrNumber(transfer_queue + "_FileTransferMBWaitingToUpload", mb_for_upload))
	{
		dprintf(D_FULLDEBUG, "Unable to determine MB waiting to upload for transfer queue %s.\n", transfer_queue.c_str());
		//return true;
	}
	long bandwidth_down_mbps, bandwidth_up_mbps;
	long bandwidth_down_bps, bandwidth_up_bps;
	double bandwidth_down_bps_recent_max;
	double bandwidth_up_bps_recent_max;
	int num_max_uploading, num_max_downloading;
	int active_num_uploading = 0;
	int active_num_downloading = 0;
	int num_wait_uploading = 0;
	int num_wait_downloading = 0;
	if (!EstimateNetworkBandwidth(scheddAd, machineAd, bandwidth_down_mbps, bandwidth_up_mbps))
	{
		// estimate file transfer time (include wait in file transfer queue and actual file transfer) using file transfer queue statistics
		// First check the current number of active file transfers and MAX_CONCURRENT_FILE_TRANSFER
		if(scheddAd.EvaluateAttrNumber(ATTR_TRANSFER_QUEUE_MAX_UPLOADING, num_max_uploading))
		{
			dprintf(D_FULLDEBUG, "Max concurrent upload file transfer is %d.\n", num_max_uploading);
		}
		else
		{
			num_max_uploading = param_integer("MAX_CONCURRENT_UPLOADS", 10);
			dprintf(D_FULLDEBUG, "Unable to get max concurrent upload from Schedd Ad, read from configs to be %d.\n", num_max_uploading);
		}
		if(scheddAd.EvaluateAttrNumber(ATTR_TRANSFER_QUEUE_MAX_DOWNLOADING, num_max_downloading))
		{
			dprintf(D_FULLDEBUG, "Max concurrent download file transfer is %d.\n", num_max_downloading);
		}
		else
		{
			num_max_uploading = param_integer("MAX_CONCURRENT_DOWNLOADS", 10);
			dprintf(D_FULLDEBUG, "Unable to get max concurrent download from Schedd Ad, read from configs to be %d.\n", num_max_downloading);
		}
		if(scheddAd.EvaluateAttrNumber(transfer_queue + "_" + ATTR_TRANSFER_QUEUE_NUM_UPLOADING, active_num_uploading))
		{
			dprintf(D_FULLDEBUG, "The number of active file transfer uploads for transfer queue %s is %d.\n", transfer_queue.c_str(), active_num_uploading);
		}
		if(scheddAd.EvaluateAttrNumber(transfer_queue + "_" + ATTR_TRANSFER_QUEUE_NUM_DOWNLOADING, active_num_downloading))
		{
			dprintf(D_FULLDEBUG, "The number of active file transfer downloads for transfer queue %s is %d.\n", transfer_queue.c_str(), active_num_downloading);
		}
		if(scheddAd.EvaluateAttrNumber(transfer_queue + "_" + ATTR_TRANSFER_QUEUE_NUM_WAITING_TO_UPLOAD, num_wait_uploading))
		{
			dprintf(D_FULLDEBUG, "The number of file transfer wait to be uploaded for transfer queue %s is %d.\n", transfer_queue.c_str(), num_wait_uploading);
		}
		if(scheddAd.EvaluateAttrNumber(transfer_queue + "_" + ATTR_TRANSFER_QUEUE_NUM_WAITING_TO_DOWNLOAD, num_wait_downloading))
		{
			dprintf(D_FULLDEBUG, "The number of file transfer wait to be downloaded for transfer queue %s is %d.\n", transfer_queue.c_str(), num_wait_downloading);
		}

		if(scheddAd.EvaluateAttrNumber(transfer_queue + "_FileTransferUploadBytesPerSecond_RecentMax", bandwidth_up_bps_recent_max) && bandwidth_up_bps_recent_max > 1e-5)
		{
			dprintf(D_FULLDEBUG, "File transfer upload bytes per second recent max in past 1 hour for transfer queue %s is %f.\n", transfer_queue.c_str(), bandwidth_up_bps_recent_max);

		}
		else
		{
			bandwidth_up_bps_recent_max  = (double)param_integer(("BANDWIDTH_UP_MBPS_" + transfer_queue).c_str(), 1000)/8*1000000;
			dprintf(D_FULLDEBUG, "Could not retrive recent max rate for file transfer upload in transfer queue %s, read from configuration as %f.\n", 
				transfer_queue.c_str(), 
				bandwidth_up_bps_recent_max);
		}
		if(scheddAd.EvaluateAttrNumber(transfer_queue + "_FileTransferDownloadBytesPerSecond_RecentMax", bandwidth_down_bps_recent_max) && bandwidth_down_bps_recent_max > 1e-5)
		{
			dprintf(D_FULLDEBUG, "File transfer download bytes per second recent max in past 1 hour for transfer queue %s is %f.\n", transfer_queue.c_str(), bandwidth_down_bps_recent_max);
		}
		else
		{
			bandwidth_down_bps_recent_max  = (double)param_integer(("BANDWIDTH_DOWN_MBPS_" + transfer_queue).c_str(), 1000)/8*1000000;
			dprintf(D_FULLDEBUG, "Could not retrive recent max rate for file transfer download in transfer queue %s, read from configuration as %f.\n", 
				transfer_queue.c_str(), 
				bandwidth_down_bps_recent_max);
		}
	}
	// Use the active file transfers for this transfer queue, max transfer rate for this transfer queue in the past one hour
	// and also the size of file transfers wait to be uploaded to estimate the upload transfer time for this job
	int transfer_input_size_mb;
	if(jobAd.EvaluateAttrNumber("TransferInputSizeMB", transfer_input_size_mb))
	{
		dprintf(D_FULLDEBUG, "Transfer input size is %d MB.\n", transfer_input_size_mb);
	}
	else
	{
		transfer_input_size_mb = 0;
		dprintf(D_FULLDEBUG, "Cannot retrieve input file transfer sandbox size, assume zero input.\n");
	}
	long est_actual_upload_file_transfer_time = (long) (transfer_input_size_mb / (bandwidth_up_bps_recent_max/1000000/(active_num_uploading+1)));
	long est_wait_in_u_transfer_queue_time  = (long) ((mb_for_upload*0.5) / (bandwidth_up_bps_recent_max/1000000));
	long est_upload_time = est_wait_in_u_transfer_queue_time + est_actual_upload_file_transfer_time;
	std::string Compute_Site;
	if (machineAd.EvaluateAttrString("Glidein_CMSSite", Compute_Site))
	{
		dprintf(D_FULLDEBUG, "Compute site is: %s.\n", Compute_Site.c_str());
	}
	dprintf(D_ALWAYS, "Estimated upload file transfer time to site %s is %ld.\n", Compute_Site.c_str(), est_upload_time);
	machineAd.InsertAttr("EstimatedUploadFileTransferTime", est_upload_time);
	bool match_with_network_threshold = param_boolean("MATCH_WITH_NETWORK_THRESHOLD", false);
	if (match_with_network_threshold) {
		long max_wait = param_integer("MAX_ESTIMATED_NETWORK_WAIT", 1000);
		if (max_wait < est_upload_time) {
			dprintf(D_FULLDEBUG, "Rejecting match due to insufficient upload bandwidth. Estimated wait is %ld of max %ld.\n", est_upload_time, max_wait);
			return false;
		}
	}
	return true;
}

bool PopulateDefaultNetworkAd(classad::ClassAd &ad)
{
	std::vector<NetworkDeviceInfo> devices;
	if (!sysapi_get_network_device_info(devices))
	{
		dprintf(D_ALWAYS, "Unable to determine local network devices\n");
		return false;
	}
	std::vector<NetworkDeviceInfo>::const_iterator it;
	int min_speed = -1;
	for (it = devices.begin(); it != devices.end(); it++)
	{
		if (strcmp(it->name(), "lo")) { continue; }
		unsigned speed = it->link_speed_mbps();
		if (!speed) { continue; }
		if ( (min_speed == -1) || (speed < min_speed)) { min_speed = speed; }
	}
	unsigned up_speed = param_integer("ESTIMATED_BANDWIDTH_UP_MBPS", 0);
	unsigned down_speed = param_integer("ESTIMATED_BANDWIDTH_DOWN_MBPS", 0);
	ad.InsertAttr("BandwidthUpMbps", static_cast<int>((up_speed > 0) ? up_speed : ((min_speed > 0) ? min_speed: 1000)));
	ad.InsertAttr("BandwidthDownMbps", static_cast<int>((down_speed > 0) ? down_speed : ((min_speed > 0) ? min_speed: 1000)));

	return true;
}

