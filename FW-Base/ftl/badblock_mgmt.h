#ifndef _BADBLOCK_MGMT_H_
#define _BADBLOCK_MGMT_H_


// bbt unit, 
/*typedef struct {
    u8 pl   : PL_BITS;
    u8 ch   :
    u8 lun;
    u16 blk;
} bbt_uint;bad
*/ 

extern u16 *get_blk_bbt_base(u16 blk);
extern bool get_lastn_good_die(u16 blk, u16 n, ppa_t *result);



#endif
