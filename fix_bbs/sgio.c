#include "fix_sector.h"
#include <scsi/sg.h>
#include <asm/byteorder.h>

#define ATA_OP_IDENTIFY (0xec)

#define SG_CHECK_CONDITION 0x02
#define SG_DRIVER_SENSE 0x08
#define SG_ATA_16_LEN 16
#define SG_ATA_16 0x85

#define SG_DXFER_NONE -1
#define SG_DXFER_TO_DEV -2
#define SG_DXFER_FROM_DEV -3
#define SG_DXFER_TO_FROM_DEV -4

enum {
	ATA_USING_LBA		= (1 << 6),
	ATA_STAT_DRQ		= (1 << 3),
	ATA_STAT_ERR		= (1 << 0),
};

struct scsi_sg_io_hdr {
	int interface_id;
	int dxfer_direction;
	unsigned char cmd_len;
	unsigned char mx_sb_len;
	unsigned short iovec_count;
	unsigned int dxfer_len;
	void *dxferp;
	unsigned char *cmdp;
	void *sbp;
	unsigned int timeout;
	unsigned int flags;
	int	pack_id;
	void *usr_ptr;
	unsigned char status;
	unsigned char masked_status;
	unsigned char msg_status;
	unsigned char sb_len_wr;
	unsigned short host_status;
	unsigned short driver_status;
	int	resid;
	unsigned int duration;
	unsigned int info;
};

struct ata_lba_regs {
	__u8	feat;
	__u8	nsect;
	__u8	lbal;
	__u8	lbam;
	__u8	lbah;
};

struct ata_tf {
	__u8 dev;
	__u8 command;
	__u8 error;
	__u8 status;
	__u8 is_lba48;
	struct ata_lba_regs	lob;
	struct ata_lba_regs	hob;
};

static __u64 tf_to_lba(struct ata_tf *tf)
{
	__u32 lba24, lbah;
	__u64 lba64;

	lba24 = (tf->lob.lbah << 16) | (tf->lob.lbam << 8) | (tf->lob.lbal);
	if (tf->is_lba48)
		lbah = (tf->hob.lbah << 16) | (tf->hob.lbam << 8) | (tf->hob.lbal);
	else
		lbah = (tf->dev & 0x0f);
	lba64 = (((__u64)lbah) << 24) | (__u64)lba24;
	return lba64;
}

static void tf_init(struct ata_tf *tf, __u8 ata_op, __u64 lba, unsigned int nsect)
{
	memset(tf, 0, sizeof(*tf));
	tf->command = ata_op;
	tf->dev = ATA_USING_LBA;
	tf->lob.lbal = lba;
	tf->lob.lbam = lba >> 8;
	tf->lob.lbah = lba >> 16;
	tf->lob.nsect = nsect;
	tf->dev |= (lba >> 24) & 0x0f;
}

static int do_drive_cmd(int fd, unsigned char *args, unsigned int timeout_secs)
{
	unsigned char cdb[SG_ATA_16_LEN];
	unsigned char sb[32], *desc;
	struct scsi_sg_io_hdr io_hdr;

	struct ata_tf tf;
	void *data = NULL;
	unsigned int data_bytes = 0;

	tf_init(&tf, args[0], 0, 0);
	tf.lob.nsect = args[1];
	tf.lob.feat = args[2];
	if (args[3]) {
		data_bytes = args[3] * 512;
		data = args + 4;
		if (!tf.lob.nsect)
			tf.lob.nsect = args[3];
	}

	//rc = sg16(fd, SG_READ, 0, &tf, data, data_bytes, timeout_secs);
	//int sg16 (int fd, int rw, int dma, struct ata_tf *tf,
	//	void *data, unsigned int data_bytes, unsigned int timeout_secs)
	memset(&cdb, 0, sizeof(cdb));
	memset(&sb, 0, sizeof(sb));
	memset(&io_hdr, 0, sizeof(struct scsi_sg_io_hdr));
	cdb[1] = (4 << 1);
	cdb[2] = 0xe;
	cdb[0] = SG_ATA_16;
	cdb[4] = tf.lob.feat;
	cdb[6] = tf.lob.nsect;
	cdb[8] = tf.lob.lbal;
	cdb[10] = tf.lob.lbam;
	cdb[12] = tf.lob.lbah;
	cdb[13] = tf.dev;
	cdb[14] = tf.command;
	io_hdr.cmd_len = SG_ATA_16_LEN;

	io_hdr.interface_id = 'S';
	io_hdr.mx_sb_len = sizeof(sb);
	io_hdr.dxfer_direction = SG_DXFER_FROM_DEV;
	io_hdr.dxfer_len = data ? data_bytes : 0;
	io_hdr.dxferp = data;
	io_hdr.cmdp = cdb;
	io_hdr.sbp = sb;
	io_hdr.pack_id = tf_to_lba(&tf);
	io_hdr.timeout = (timeout_secs ? timeout_secs : 15) * 1000;

	if (ioctl(fd, SG_IO, &io_hdr) == -1) {
		perror("ioctl(fd,SG_IO)");
		return -1;
	}

	if (io_hdr.status && io_hdr.status != SG_CHECK_CONDITION)
		return -1;
	if (io_hdr.host_status)
		return -1;
	if (io_hdr.driver_status && (io_hdr.driver_status != SG_DRIVER_SENSE))
		return -1;

	desc = sb + 8;

	tf.is_lba48 = desc[2] & 1;
	tf.error = desc[3];
	tf.lob.nsect = desc[5];
	tf.lob.lbal = desc[7];
	tf.lob.lbam = desc[9];
	tf.lob.lbah = desc[11];
	tf.dev = desc[12];
	tf.status = desc[13];
	tf.hob.feat = 0;
	tf.hob.nsect = 0;
	tf.hob.lbal = 0;
	tf.hob.lbam = 0;
	tf.hob.lbah = 0;

	if (tf.status & (ATA_STAT_ERR | ATA_STAT_DRQ))
		return -1;

	return 0;
}

int get_identify_data(int fd, __u16 *id)
{
	static __u8 args[4+512];
	int i;

	memset(args, 0, sizeof(args));
	args[0] = ATA_OP_IDENTIFY;
	args[3] = 1;	/* sector count */
	if (do_drive_cmd(fd, args, 0))
		return -1;

	/* byte-swap the little-endian IDENTIFY data to match byte-order on host CPU */
	memcpy(id, (void *)(args + 4), 512);
	for (i = 0; i < 0x100; ++i)
		__le16_to_cpus(id[i]);

	return 0;
}
