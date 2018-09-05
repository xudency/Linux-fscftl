/*
 * Copyright (C) 2018-2020 NET-Swift.
 * Initial release: Dengcai Xu <dengcaixu@net-swift.com>
 *
 * ALL RIGHTS RESERVED. These coded instructions and program statements are
 * copyrighted works and confidential proprietary information of NET-Swift Corp.
 * They may not be modified, copied, reproduced, distributed, or disclosed to
 * third parties in any manner, medium, or form, in whole or in part.
 *
 * function: 
 * read/write datapath, ingress of commmand handler
 *
 * HW notify CPU method
 *    1. HW interrupt ---> FW check Event register what happen ---> FW handle it
 *    2. FW Polling check Event register what happen ---> if yes handle it
 *
 */

#include "nvme_spec.h"
#include "msg_fmt.h"
#include "datapath.h"

// host coming nvme command
host_nvme_cmd_entry    gat_host_nvme_cmd_array[HOST_NVME_CMD_ENTRY_CNT]       = {{{0}}};

// pending due to SPM not available
struct Queue host_nvme_cmd_pend_q;

// fw internal command
fw_internal_cmd_entry    gat_fw_internal_cmd_array[FW_INTERNAL_CMD_ENTRY_CNT]       = {{{0}}};

// pending due to SPM not available
struct Queue fw_itnl_cmd_pend_q;

// phif_cmd_cpl NOT define in stack, to prevenrt re-constructed in statemachine
phif_cmd_cpl	gat_host_cmd_cpl_array[HOST_NVME_CMD_ENTRY_CNT];

struct Queue host_nvme_cpl_pend_q;

// when command process completion, this is the hook callback routine
int process_completion_task(void *para)
{
	// TODO: process all to Post CQE to Host

	yield_task(HDC, completion_prio);

	return 0;
}

void msg_header_filled(struct msg_qw0 *header, u8 cnt, u8 dstfifo, u8 dst, 
							u8 msgid, u8 tag, u8 ext_tag, u8 src, u8 sriov)
{
	header->cnt = cnt; 	// phif_cmd_req, the length is fixed on 3 QW
	header->dstfifo = dstfifo;
	header->dst = dst;
	header->prio = 0;
	header->msgid = msgid;
	header->tag = tag;
	header->ext_tag = ext_tag;
	header->src = src;
	header->vfa = sriov & 0x1;
	header->port = (sriov >>1) & 0x1;
	header->vf = (sriov >>2) & 0xf;
}

int send_phif_cmd_req(phif_cmd_req *msg)
{
	if (port_is_available()) {
		memcpy(PHIF_CMD_REQ_SPM, msg, sizeof(phif_cmd_req)); 
		return 0;
	} else {
		/*break, CPU schedule other task*/
		return 1;
	}
}

int send_phif_cmd_cpl(phif_cmd_cpl *msg)
{
	if (port_is_available()) {
		memcpy(PHIF_CMD_CPL_SPM, msg, sizeof(phif_cmd_req)); 
		return 0;
	} else {
		/*break, CPU schedule other task*/
		return 1;
	}
}

// valid: bit0:QW_PI   bit1:QW_ADDR  bit2:QW_DATA
int send_phif_wdma_req(phif_wdma_req_mandatory *m, phif_wdma_req_optional *o, u8 valid)
{
	if (port_is_available()) {
		void *ptr = (void *)PHIF_WDMA_REQ_SPM;
		memcpy(ptr, m, PHIF_WDMA_REQ_M_LEN));
		ptr += PHIF_WDMA_REQ_M_LEN;

		if (valid & WDMA_QW_PI) {
			memcpy(ptr, o, QWORD_BYTES);
			ptr += QWORD_BYTES;
			o += QWORD_BYTES;
		}

		
		if (valid & WDMA_QW_ADDR) {
			memcpy(ptr, o, QWORD_BYTES);
			ptr += QWORD_BYTES;
			o += QWORD_BYTES;
		}

		if (valid & WDMA_QW_DATA) {
			memcpy(ptr, o, QWORD_BYTES*m->control.pld_qwn);
		}

		return 0;
	} else {
		/*break, CPU schedule other task*/
		return 1;
	}
}


// move data from Cbuff to Host memory via PHIF WDMA
// beware: both cbuff and host address is continuously
int wdma_read_fwdata_to_host(u64 host_addr, u64 cbuff_addr, u16 length)
{
	phif_wdma_req_mandatory m;

	// TODO: tag allocated?  bitmap, find_first_zero_bit, u32 support 32 command is enough
	u16 tag = hdc_alloc_cmdtag();

	msg_header_filled(&m.header, 6, MSG_NID_PHIF, MSG_NID_PHIF, MSGID_PHIF_WDMA_REQ, 
					  tag, HDC_EXT_TAG, MSG_NID_HDC, 0);

	m.control.blen = length;	// length
	m.control.pld_qwn = 1;		// only one QW_ADDR, because cbuff must continuous

	m.hdata_addr = host_addr;	//host address

	phif_wdma_req_optional o;
	o.cbuff_addr = cbuff_addr;   // cbuff address

	u8 valid = WDMA_QW_ADDR;

	return send_phif_wdma_req(&m, &o, valid);
}

/*host_nvme_cmd_entry *get_host_cmd_entry(u8 tag)
{
	host_nvme_cmd_entry *entry = __get_host_cmd_entry(tag);

	if (entry->state == WRITE_FLOW_STATE_INITIAL) {
		return entry;
	} else {
		print_err("tagid duplicated!!! this tag cmd state:%d", entry->state);
		// if it occur, it should be a HW BUG
		//panic();
		return NULL;
	}
}*/

void save_in_host_cmd_entry(host_nvme_cmd_entry *entry, hdc_nvme_cmd *cmd)
{
	entry->cmd_tag = cmd->header.tag;
	entry->sqid = cmd->header.sqid;

	entry->vfa = cmd->header.vfa;
	entry->port = cmd->header.port;
	entry->vf = cmd->header.vf;

	entry->sqe = cmd->sqe;
}

// TODO:: unify cmd flow, response is a callback

// write/read LBA(HW accelerate) command complete
void phif_cmd_response_to_hdc(void)
{
	phif_cmd_rsp *rsp = (phif_cmd_rsp *)PHIF_CMD_RSP_SPM;

	host_nvme_cmd_entry *host_cmd_entry = __get_host_cmd_entry(rsp->tag);

	host_cmd_entry->sta_sct = rsp->sta_sct;
	host_cmd_entry->sta_sc = rsp->sta_sc;


	switch (host_cmd_entry->sqe.common.opcode) {
	case nvme_io_write:
		host_cmd_entry.state = WRITE_FLOW_STATE_HAS_PHIF_RSP;
		write_datapath_hdc(host_cmd_entry);
		break;
	case nvme_io_read:
		host_cmd_entry.state = READ_FLOW_STATE_HAS_PHIF_RSP;
		read_datapath_hdc(host_cmd_entry);
		break;
	}

}

// move cbuff data to host complete
void phif_wdma_response_to_hdc(void)
{
	phif_wdma_rsp *rsp = (phif_wdma_rsp *)PHIF_WDMA_RSP_SPM;

	// release this HDC cmd_tag allocated before
	u16 itnl_tag = rsp->tag;

	fw_internal_cmd_entry *fw_cmd_entry = __get_fw_cmd_entry(itnl_tag);
	host_nvme_cmd_entry *host_cmd_entry = __get_host_cmd_entry(fw_cmd_entry->host_tag);

	// any chunk error, this host command is error
	if (rsp->staus) {
		host_cmd_entry->sta_sc = NVME_SC_INTERNAL;
		// reset HW
	}
	
	if (--host_cmd_entry->ckc) {
		// prp1 part and prp2 part all response, structured a phif_cmd_cpl to PHIF
		phif_cmd_cpl cpl;
		setup_phif_cmd_cpl(&cpl, host_cmd_entry);
		send_phif_cmd_cpl(&cpl);
	}

	hdc_free_cmdtag(itnl_tag);
}


// host datapath prepare phif_cmd_req
void dp_setup_phif_cmd_req(phif_cmd_req *req, host_nvme_cmd_entry *host_cmd_entry)
{
	u8 flbas, lbaf_type;
	u16 lba_size;	// in byte
	ctrl_dw12h_t control;
	u8 opcode = host_cmd_entry->sqe.rw.opcode;
	u64 start_lba = host_cmd_entry->sqe.rw.slba;
	u32 nsid = host_cmd_entry->sqe.rw.nsid;
	struct nvme_lbaf *lbaf;
	control.ctrl = host_cmd_entry->sqe.rw.control;
	struct nvme_id_ns *ns_info;
	host_nvme_cmd_entry *host_cmd_entry;

	//QW0
	req->header.cnt = 2; 	// phif_cmd_req, the length is fixed on 3 QW
	req->header.dstfifo = MSG_NID_PHIF;
	req->header.dst = MSG_NID_PHIF;
	req->header.prio = 0;
	req->header.msgid = MSGID_PHIF_CMD_REQ;

	// for host cmd, we support max 256 outstanding commands, tag 8 bit is enough
	// EXTAG no used, must keep it as 0, tag copy from hdc_nvme_cmd which is assign by PHIF
	req->header.tag = host_cmd_entry->cmd_tag;
	req->header.ext_tag = PHIF_EXTAG;
	req->header.src = MSG_NID_HDC;
	req->header.vfa = host_cmd_entry->vfa;
	req->header.port = host_cmd_entry->port;
	req->header.vf = host_cmd_entry->vf;
	req->header.sqid = host_cmd_entry->sqid;

	// TODO: Reservation check, if this is reservation conflict
	
	
	// TODO: TCG support, LBA Range, e.g. LBA[0 99] key1,  LBA[100 199] key2 .....
	req->header.hxts_mode = enabled | key; 
	
	ns_info = get_identify_ns(nsid);
	
	//QW1	
	req->cpa = start_lba / 1;
	flbas = ns_info->flbas;
	lbaf_type = flbas & NVME_NS_FLBAS_LBA_MASK;
	lbaf = &ns_info->lbaf[lbaf_type];

	req->hmeta_size = lbaf->ms / 8;
	req->cph_size = lbaf->cphs;

	lba_size = 1 << lbaf->ds;
	if (lba_size == 512) {
		req->lb_size = 0;
	} else if (lba_size == 4096) {
		req->lb_size = 1;
	} else if (lba_size == 16384) {
		req->lb_size = 2;
	} else {
		print_err("LBA Size:%d not support", lba_size);
	}
	
	req->crc_en = 1;
	req->dps = ns_info->dps;  // PI type and PI in meta pos, first or last 8B
	req->flbas = flbas;       // DIX or DIF / format LBA size use which (1 of the 16) 
	req->cache_en = !control.bits.fua;

	// TODO:: write directive, 
	if (opcode == nvme_io_read) {
		//Latency Control via Queue schedule in FQMGR(9 queue per die)
		
		dsm_dw13_t dsmgmt;
		dsmgmt.dw13 = host_cmd_entry->sqe.rw.dsmgmt;
		u8 access_lat = dsmgmt.bits.dsm_latency;
		switch (access_lat) {
		case NVME_RW_DSM_LATENCY_NONE:
			// XXX: if no latency info provide, add to which queue ?
			req.band_rdtype = FQMGR_HOST_READ1;
			break;	
		case NVME_RW_DSM_LATENCY_IDLE:
			req.band_rdtype = FQMGR_HOST_READ2;
			break;
		case NVME_RW_DSM_LATENCY_NORM:
			req.band_rdtype = FQMGR_HOST_READ1;
			break;
		case NVME_RW_DSM_LATENCY_LOW:
			req.band_rdtype = FQMGR_HOST_READ0;
			break;
		default:
			print_err("access latency Invalid");
			return - EINVAL;
		}

	
	} else (opcode == nvme_io_write) {		
		req->band_rdtype = HOSTBAND;
	}


	//QW2
	req->elba = (nsid<<32) | start_lba;

	// ask hw to export these define in .hdl or .rdl
}


/*void setup_phif_cmd_cpl(phif_cmd_cpl *cpl, u8 tag, u8 sqid, u8 sriov, u8 sta_sct, u8 sta_sc)
{
	// QW0
	cpl->header.cnt = 2;
	cpl->header.dstfifo = ;
	cpl->header.dst = MSG_NID_PHIF;
	cpl->header.prio = 0;
	cpl->header.msgid = MSGID_PHIF_CMD_REQ;
	cpl->header.tag = tag;
	cpl->header.ext_tag = PHIF_EXTAG;
	cpl->header.src = MSG_NID_HDC;
	cpl->header.vfa = sriov & 0x1;
	cpl->header.port = (sriov >>1) & 0x1;
	cpl->header.vf = (sriov >>2) & 0xf;	
	cpl->header.sqid = sqid;

	cpl->cqe.result = 0; // this is command specified
	cpl->cqe.status = (sta_sc<<1) | (>sta_sct<<9);
	// phase is filled by HW
}*/



void setup_phif_cmd_cpl(phif_cmd_cpl *cpl, host_nvme_cmd_entry *host_cmd_entry)
{
	// QW0
	cpl->header.cnt = 2;
	cpl->header.dstfifo = ;
	cpl->header.dst = MSG_NID_PHIF;
	cpl->header.prio = 0;
	cpl->header.msgid = MSGID_PHIF_CMD_REQ;
	cpl->header.tag = host_cmd_entry->cmd_tag;
	cpl->header.ext_tag = PHIF_EXTAG;
	cpl->header.src = MSG_NID_HDC;
	cpl->header.vfa = host_cmd_entry->vfa;
	cpl->header.port = host_cmd_entry->port;
	cpl->header.vf = host_cmd_entry->vf;
	cpl->header.sqid = host_cmd_entry->sqid;

	cpl->cqe.result = 0; // this is command specified
	cpl->cqe.status = (host_cmd_entry->sta_sc<<1) | (host_cmd_entry->sta_sct<<9);
	// phase is filled by HW
}


host_nvme_cmd_entry *get_next_host_cmd_entry(void)
{
	host_nvme_cmd_entry *host_cmd_entry = NULL;

	if (queue_empty(&host_nvme_cpl_pend_q)) {
		host_cmd_entry = dequeue(&host_nvme_cmd_pend_q);
	} else {
		host_cmd_entry = dequeue(&host_nvme_cpl_pend_q);
	}

	return host_cmd_entry;
}

// Process Host IO Comamnd
void handle_nvme_io_command(hdc_nvme_cmd *cmd)
{
	u8 opcode = cmd->sqe.common.opcode;
	u64 start_lba = cmd->sqe.rw.slba;
	u16 nlb = cmd->sqe.rw.length;
	u32 nsid = cmd->sqe.rw.nsid;
	u8 tag = cmd->header.tag;
	
	// tag is assigned by PHIF, it can guarantee this tag is free,
	host_nvme_cmd_entry *host_cmd_entry = __get_host_cmd_entry(tag);

	save_in_host_cmd_entry(host_cmd_entry, cmd);

	// para check
	if ((start_lba + nlb) > MAX_LBA) {
		print_err("the write LBA Range[%lld--%lld] exceed max_lba:%d", start_lba, start_lba+nlb, MAX_LBA);
		host_cmd_entry->sta_sct = NVME_SCT_GENERIC;
		host_cmd_entry->sta_sc = NVME_SC_LBA_RANGE;
		goto cmd_quit;	
	}

	if (nsid > MAX_NSID) {
		print_err("NSID:%d is Invalid", nsid);
		host_cmd_entry->sta_sct = NVME_SCT_GENERIC;
		host_cmd_entry->sta_sc = NVME_SC_INVALID_NS;
		goto cmd_quit;	
	}

	enqueue(&host_nvme_cmd_pend_q, host_cmd_entry->next);

	if (opcode == nvme_io_read) {
		host_cmd_entry->state = READ_FLOW_STATE_QUEUED;
	} else if (opcode == nvme_io_write) {
		host_cmd_entry->state = WRITE_FLOW_STATE_QUEUED;
	}

	host_cmd_entry = get_next_host_cmd_entry();
	
	assert(host_cmd_entry);
	
	switch (opcode) {
	case nvme_io_read:
		read_datapath_hdc(host_cmd_entry);
		break;
	case nvme_io_write:
		write_datapath_hdc(host_cmd_entry);
		break;
	case nvme_io_flush:
		break;
	case nvme_io_compare:
		break;
	default:
		print_err("Opcode:0x%x Invalid", opcode);
		host_cmd_entry->sta_sct = NVME_SCT_GENERIC;
		host_cmd_entry->sta_sc = NVME_SC_INVALID_OPCODE;
		goto cmd_quit;
	}

	return;    // host command in-pocess 1.wait phif_cmd_rsp,  2.wait SPM available

cmd_quit:
	// this NVMe Command is Invalid, post CQE to host immediately
	phif_cmd_cpl *cpl = __get_host_cmd_cpl_entry(tag)
	setup_phif_cmd_cpl(cpl, host_cmd_entry);
	if (send_phif_cmd_cpl(cpl)) {
		enqueue_front(host_nvme_cpl_pend_q, host_cmd_entry->next);
	}

	return;
}

// when host prepare a SQE and submit it to the SQ, then write SQTail DB
// Phif fetch it and save in CMD_TABLE, then notify HDC by message hdc_nvme_cmd

// taskfn demo
void hdc_host_cmd_task(void *para)
{
	//hdc_nvme_cmd *cmd = (hdc_nvme_cmd *)para;
	hdc_nvme_cmd *cmd = (hdc_nvme_cmd *)HDC_NVME_CMD_SPM;

	if (cmd->header.sqid == 0) {
		// admin queue, this is admin cmd
		handle_nvme_admin_command(cmd);
	} else {
		// io command
		handle_nvme_io_command(cmd);
	}

	return;
}

