//           vioblk.c - VirtIO serial port (console)
//

#include "virtio.h"
#include "intr.h"
#include "halt.h"
#include "heap.h"
#include "io.h"
#include "device.h"
#include "error.h"
#include "string.h"
#include "thread.h"
#include "lock.h"

//           COMPILE-TIME PARAMETERS
//

#define VIOBLK_IRQ_PRIO 1

//           INTERNAL CONSTANT DEFINITIONS
//

//           VirtIO block device feature bits (number, *not* mask)

#define VIRTIO_BLK_F_SIZE_MAX       1
#define VIRTIO_BLK_F_SEG_MAX        2
#define VIRTIO_BLK_F_GEOMETRY       4
#define VIRTIO_BLK_F_RO             5
#define VIRTIO_BLK_F_BLK_SIZE       6
#define VIRTIO_BLK_F_FLUSH          9
#define VIRTIO_BLK_F_TOPOLOGY       10
#define VIRTIO_BLK_F_CONFIG_WCE     11
#define VIRTIO_BLK_F_MQ             12
#define VIRTIO_BLK_F_DISCARD        13
#define VIRTIO_BLK_F_WRITE_ZEROES   14

//           INTERNAL TYPE DEFINITIONS
//

//           All VirtIO block device requests consist of a request header, defined below,
//           followed by data, followed by a status byte. The header is device-read-only,
//           the data may be device-read-only or device-written (depending on request
//           type), and the status byte is device-written.

static struct lock vioblk_lock;
struct vioblk_request_header {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};

//           Request type (for vioblk_request_header)

#define VIRTIO_BLK_T_IN             0
#define VIRTIO_BLK_T_OUT            1

//           Status byte values

#define VIRTIO_BLK_S_OK         0
#define VIRTIO_BLK_S_IOERR      1
#define VIRTIO_BLK_S_UNSUPP     2

//           Main device structure.
//
//           FIXME You may modify this structure in any way you want. It is given as a
//           hint to help you, but you may have your own (better!) way of doing things.

struct vioblk_device {
    volatile struct virtio_mmio_regs * regs;
    struct io_intf io_intf;
    uint16_t instno;
    uint16_t irqno;
    int8_t opened;
    int8_t readonly;

    //           optimal block size
    uint32_t blksz;
    //           current position
    uint64_t pos;
    //           sizeo of device in bytes
    uint64_t size;
    //           size of device in blksz blocks
    uint64_t blkcnt;

    struct {
        //           signaled from ISR
        struct condition used_updated;

        //           We use a simple scheme of one transaction at a time.

        union {
            struct virtq_avail avail;
            char _avail_filler[VIRTQ_AVAIL_SIZE(1)];
        };

        union {
            volatile struct virtq_used used;
            char _used_filler[VIRTQ_USED_SIZE(1)];
        };

        //           The first descriptor is an indirect descriptor and is the one used in
        //           the avail and used rings. The second descriptor points to the header,
        //           the third points to the data, and the fourth to the status byte.

        struct virtq_desc desc[4];
        struct vioblk_request_header req_header;
        uint8_t req_status;
    } vq;

    //           Block currently in block buffer
    uint64_t bufblkno;
    //           Block buffer
    char * blkbuf;
};

//           INTERNAL FUNCTION DECLARATIONS
//

static int vioblk_open(struct io_intf ** ioptr, void * aux);

static void vioblk_close(struct io_intf * io);

static long vioblk_read (
    struct io_intf * restrict io,
    void * restrict buf,
    unsigned long bufsz);

static long vioblk_write (
    struct io_intf * restrict io,
    const void * restrict buf,
    unsigned long n);

static int vioblk_ioctl (
    struct io_intf * restrict io, int cmd, void * restrict arg);

static void vioblk_isr(int irqno, void * aux);

//           IOCTLs

static int vioblk_getlen(const struct vioblk_device * dev, uint64_t * lenptr);
static int vioblk_getpos(const struct vioblk_device * dev, uint64_t * posptr);
static int vioblk_setpos(struct vioblk_device * dev, const uint64_t * posptr);
static int vioblk_getblksz (
    const struct vioblk_device * dev, uint32_t * blkszptr);

//           EXPORTED FUNCTION DEFINITIONS
//
//           Attaches a VirtIO block device. Declared and called directly from virtio.c.


//input: registor: regs, int irqno
//output: null
//function: Initializes the virtio block device with the necessary IO operation functions and sets the required
//feature bits
void vioblk_attach(volatile struct virtio_mmio_regs * regs, int irqno) {
    //           FIXME add additional declarations here if needed
    static struct io_ops blk_ops = {       //block operation
        .close = vioblk_close,
        .read = vioblk_read,
        .write = vioblk_write,
        .ctl = vioblk_ioctl
    };
    lock_init(&vioblk_lock, "vioblk");
    virtio_featset_t enabled_features, wanted_features, needed_features;
    struct vioblk_device * dev;
    uint_fast32_t blksz;
    int result;

    //assert (regs->device_id == VIRTIO_ID_BLOCK);

    //           Signal device that we found a driver

    regs->status |= VIRTIO_STAT_DRIVER;
    //           fence o,io
    __sync_synchronize();


    //           Negotiate features. We need:
    //            - VIRTIO_F_RING_RESET and
    //            - VIRTIO_F_INDIRECT_DESC
    //           We want:
    //            - VIRTIO_BLK_F_BLK_SIZE and
    //            - VIRTIO_BLK_F_TOPOLOGY.

    virtio_featset_init(needed_features); //4.1 features negotiated
    virtio_featset_add(needed_features, VIRTIO_F_RING_RESET);
    virtio_featset_add(needed_features, VIRTIO_F_INDIRECT_DESC);
    virtio_featset_init(wanted_features);
    virtio_featset_add(wanted_features, VIRTIO_BLK_F_BLK_SIZE);
    virtio_featset_add(wanted_features, VIRTIO_BLK_F_TOPOLOGY);
    result = virtio_negotiate_features(regs,
        enabled_features, wanted_features, needed_features);

    if (result != 0) {
        kprintf("%p: virtio feature negotiation failed\n", regs);
        return;
    }

    //           If the device provides a block size, use it. Otherwise, use 512.

    if (virtio_featset_test(enabled_features, VIRTIO_BLK_F_BLK_SIZE))
        blksz = regs->config.blk.blk_size;
    else
        blksz = 512;
    debug("%p: virtio block device block size is %lu", regs, (long)blksz);

    //           Allocate initialize device struct

    dev = kmalloc(sizeof(struct vioblk_device) + blksz);
    memset(dev, 0, sizeof(struct vioblk_device));

    //           FIXME Finish initialization of vioblk device here
    dev->regs = regs;         //initialize device
    dev->irqno = irqno;
    dev->blksz = blksz;
    dev->blkcnt = regs->config.blk.capacity;
    dev->size = regs->config.blk.capacity* dev->blksz;
    dev->blkbuf = (char*)&(dev->blkbuf) + sizeof(char*);
    dev->io_intf.ops = &blk_ops;
    //attach avail and used struct
    virtio_attach_virtq (regs, 0, 1, (uint64_t)(&dev->vq.desc), (uint64_t)(&dev->vq.used), (uint64_t)(&dev->vq.avail));
    //fills out descriptor
    dev->vq.desc[0].addr = (uint64_t)&dev->vq.desc[1];
    dev->vq.desc[0].len = 3 * sizeof(dev->vq.desc);
    dev->vq.desc[0].flags |= VIRTQ_DESC_F_INDIRECT;
    dev->vq.desc[3].next = -1;
    dev->vq.desc[1].addr = (uint64_t)&dev->vq.req_header;
    dev->vq.desc[1].len = sizeof(dev->vq.req_header);
    dev->vq.desc[1].flags |= VIRTQ_DESC_F_NEXT;
    dev->vq.desc[1].next = 1;
    dev->vq.desc[2].addr = (uint64_t)dev->blkbuf;
    dev->vq.desc[2].len = blksz;
    dev->vq.desc[2].flags |= VIRTQ_DESC_F_NEXT;
    dev->vq.desc[2].next = 2;
    dev->vq.desc[3].addr = (uint64_t)&dev->vq.req_status;
    dev->vq.desc[3].len = sizeof(dev->vq.req_status);
    dev->vq.desc[3].flags |= VIRTQ_DESC_F_WRITE;
    dev->vq.desc[3].next = 0;

    intr_register_isr(irqno, 1, &vioblk_isr, dev);   //interrupt service routine
    dev->instno = device_register("blk", &vioblk_open, dev); //register device
    condition_init(&dev->vq.used_updated, "used_updated");
    regs->status |= VIRTIO_STAT_DRIVER_OK;
    //           fence o,oi
    __sync_synchronize();

}

//input: pointer to pointer ioptr, void* aux
//output: 0 for sucess
//function: Sets the virtq avail and virtq used queues such that they are available for use.
int vioblk_open(struct io_intf ** ioptr, void * aux) {
    //           FIXME your code here
    struct vioblk_device *dev = aux;
    if(dev->opened == 1){           //fail case
        return -EBUSY;
    }
    virtio_enable_virtq(dev->regs,0);
    dev->vq.avail.idx = 0;         //set flags
    dev->vq.used.idx = 0;
    dev->vq.avail.flags = 0;
    dev->vq.used.flags = 0;
    dev->opened = 1;
    dev->readonly = (dev->regs->config.blk.writeback == 0);
    intr_enable_irq(dev->irqno);
    dev->io_intf.refcnt = 1;
    *ioptr = &dev->io_intf;
    return 0;
}

//           Must be called with interrupts enabled to ensure there are no pending
//           interrupts (ISR will not execute after closing).

//input: pointer io
//output: 0 for sucess
//function: Resets the virtq avail and virtq used queues and sets necessary flags in vioblk device.
void vioblk_close(struct io_intf * io) {
    //           FIXME your code here
    struct vioblk_device *const dev = (void*)io - offsetof(struct vioblk_device, io_intf);
    dev->vq.avail.idx = 0;                //set flags
    dev->vq.used.idx = 0;
    dev->vq.avail.flags = VIRTQ_AVAIL_F_NO_INTERRUPT;
    dev->vq.used.flags = VIRTQ_USED_F_NO_NOTIFY;
    virtio_reset_virtq(dev->regs,0);
    dev->opened = 0;
    intr_disable_irq(dev->irqno);

}

//input: pointer io, pointer buf, long int bufsz
//output: number of bytes read
//function:Reads bufsz number of bytes from the disk and writes them to buf
long vioblk_read (
    struct io_intf * restrict io,
    void * restrict buf,
    unsigned long bufsz)
{
    //           FIXME your code here
    lock_acquire(&vioblk_lock);
    struct vioblk_device *const dev = ((void*)io - offsetof(struct vioblk_device, io_intf));
    if (dev->size < dev->pos + bufsz) {
        bufsz = dev->size - dev->pos;
    }
    unsigned long counter = 0;
    unsigned long number;
    if(!dev->opened){            //not open
        return -EBADFMT;
    }
    uint64_t pos;
    if(vioblk_getpos(dev, &pos)!=0) {   //check position
        return -EIO;
    }
    int64_t  sec= dev->pos / dev->blksz;   //sector
    while(counter < bufsz){
        dev->vq.req_header.type = VIRTIO_BLK_T_IN;
        dev->vq.req_header.sector = sec;
        dev->vq.desc[2].flags |= VIRTQ_DESC_F_WRITE;
        if(dev->blksz < bufsz - counter){   //get number of btes to read
            number = dev ->blksz;
        }
        else{
            number = bufsz - counter;
        }
        dev->vq.avail.ring[dev->vq.avail.idx % 1] = 0;  //request for available ring
        dev->vq.avail.idx++;
        virtio_notify_avail(dev->regs, 0);
        int dis = intr_disable();
        while(dev->vq.used.idx != dev->vq.avail.idx){   //wait condition
            condition_wait(&dev->vq.used_updated);
        }
        intr_restore(dis);
        if(dev->vq.req_status != VIRTIO_BLK_S_OK){   // check status
            return -EIO;
        }
        memcpy(buf + counter, dev->blkbuf+ pos % dev->blksz, number);
        counter += number;
        sec++;
        pos += number;
        if(vioblk_setpos(dev, &pos)!=0){       //check position
        return -EIO;
        }
    }
    lock_release(&vioblk_lock);
    return counter;


    
}


//input: pointer io, pointer buf, long int n
//output: number of bytes write
//function: Writes n number of bytes from the parameter buf to the disk
long vioblk_write(struct io_intf *io,
    const void *buf,
    size_t n)
{
    lock_acquire(&vioblk_lock);
    struct vioblk_device *dev = (void*)io - offsetof(struct vioblk_device, io_intf);
    if (dev->size < dev->pos + n)
        n = dev->size - dev->pos;
    uint64_t bytes_written = 0;
    uint64_t remain = n;
    if (!dev->opened)                          //not open
        return -EBADFMT;
    uint64_t pos;
    if (vioblk_getpos(dev, &pos) != 0)         //get position
        return -EIO;
    while(remain > 0){
        uint64_t block_pos = pos - (pos % dev->blksz);
        uint64_t write_size = dev->blksz - pos % dev->blksz;
        dev->vq.req_header.type = VIRTIO_BLK_T_OUT;
        dev->vq.req_header.sector = block_pos / dev->blksz;
        dev->vq.desc[2].flags &= ~VIRTQ_DESC_F_WRITE;
        if (write_size > remain)
            write_size = remain;
        memcpy(dev->blkbuf + pos % dev->blksz, buf + bytes_written, write_size);  //memory copy
        dev->vq.avail.ring[dev->vq.avail.idx % 1] = 0;
        dev->vq.avail.idx++;
        virtio_notify_avail(dev->regs, 0);
        int dis = intr_disable();                 //wait for updated
        while (dev->vq.used.idx != dev->vq.avail.idx){
            condition_wait(&dev->vq.used_updated);
        }
        intr_restore(dis);
        if (dev->vq.req_status != VIRTIO_BLK_S_OK)
            return -EIO;
        pos += write_size;
        bytes_written += write_size;
        remain -= write_size;
        if (vioblk_setpos(dev, &pos) != 0)
            return -EIO;
    }
    lock_release(&vioblk_lock);
    return bytes_written;
}



//input: pointer io, int cmd, pointer arg
//output: various ioctl
//function: swtich to different cases according to command
int vioblk_ioctl(struct io_intf * restrict io, int cmd, void * restrict arg) {
    struct vioblk_device * const dev = (void*)io - offsetof(struct vioblk_device, io_intf);
    trace("%s(cmd=%d,arg=%p)", __func__, cmd, arg);
    switch (cmd) {
    case IOCTL_GETLEN:
        return vioblk_getlen(dev, arg);
    case IOCTL_GETPOS:
        return vioblk_getpos(dev, arg);
    case IOCTL_SETPOS:
        return vioblk_setpos(dev, arg);
    case IOCTL_GETBLKSZ:
        return vioblk_getblksz(dev, arg);
    default:
        return -ENOTSUP;
    }
}
//input: int irqno, pointer aux
//output: null
//function: Sets the appropriate device registers and wakes the thread up after waiting for the disk to finish servicing a request.
void vioblk_isr(int irqno, void * aux) {
    //           FIXME your code here
    struct vioblk_device *dev = aux;
    dev->regs->interrupt_ack = dev->regs->interrupt_status;     //set ack to status
    condition_broadcast(&dev->vq.used_updated);      //wakes up thread

}

//input: pointer dev, pointer lenptr
//output: 0 for success, -ENIVAL for failure
//function: Ioctl helper function which provides the device size in bytes
int vioblk_getlen(const struct vioblk_device * dev, uint64_t * lenptr) {
    //           FIXME your code here
    if(!dev || !lenptr){   //fail case
        return -EINVAL;
    }
    *lenptr = dev->size;     //get size
    return 0;
}

//input: pointer dev, pointer posptr
//output: 0 for success, -ENIVAL for failure
//function: Ioctl helper function which gets the current position in the disk which is currently being written to or
//read from
int vioblk_getpos(const struct vioblk_device * dev, uint64_t * posptr) {
    //           FIXME your code here
    if(!dev || !posptr){    //fail case
        return -EINVAL;
    }
    *posptr = dev->pos;   //get the position
    return 0;
}

//input: pointer dev, pointer posptr
//output: 0 for success, -ENIVAL for failure
//function: Ioctl helper function which sets the current position in the disk which is currently being written to or
//read from.
int vioblk_setpos(struct vioblk_device * dev, const uint64_t * posptr) {
    //           FIXME your code here
    if(!dev || !posptr || *posptr > dev->size){     //fail case
        return -EINVAL;
    }
    dev->pos = *posptr;      //set position
    return 0;
}

//input: pointer dev, pointer blkszptr
//output: 0 for success, -ENIVAL for failure
//function: Ioctl helper function which provides the device block size.
int vioblk_getblksz (
    const struct vioblk_device * dev, uint32_t * blkszptr)
{
    //           FIXME your code here
    if(!dev || !blkszptr){     //fail case
        return -EINVAL;
    }
    *blkszptr = dev->blksz;    //get the block size
    return 0;
}
