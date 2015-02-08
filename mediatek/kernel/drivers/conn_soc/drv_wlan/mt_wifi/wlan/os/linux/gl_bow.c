





#include "gl_os.h"
#include "debug.h"
#include "wlan_lib.h"
#include "gl_wext.h"
#include "precomp.h"
#include <linux/poll.h>
#include "bss.h"

#if CFG_ENABLE_BT_OVER_WIFI

/* @FIXME if there is command/event with payload length > 28 */
#define MAX_BUFFER_SIZE         (64)



#if CFG_BOW_TEST
    UINT_32 g_u4PrevSysTime = 0;
    UINT_32 g_u4CurrentSysTime = 0;
    UINT_32 g_arBowRevPalPacketTime[11];
#endif


// forward declarations
static ssize_t
mt6620_ampc_read(
    IN struct file *filp,
    IN char __user *buf,
    IN size_t size,
    IN OUT loff_t *ppos);

static ssize_t
mt6620_ampc_write(
    IN struct file *filp,
    OUT const char __user *buf,
    IN size_t size,
    IN OUT loff_t *ppos);

static long
mt6620_ampc_ioctl(
    IN struct file *filp,
    IN unsigned int cmd,
    IN OUT unsigned long arg);

static unsigned int
mt6620_ampc_poll(
    IN struct file *filp,
    IN poll_table *wait);

static int
mt6620_ampc_open(
    IN struct inode *inodep,
    IN struct file *filp);

static int
mt6620_ampc_release(
    IN struct inode *inodep,
    IN struct file *filp);


// character file operations
static const struct file_operations mt6620_ampc_fops = {
    //.owner              = THIS_MODULE,
    .read               = mt6620_ampc_read,
    .write              = mt6620_ampc_write,
    .unlocked_ioctl     = mt6620_ampc_ioctl,
    .poll               = mt6620_ampc_poll,
    .open               = mt6620_ampc_open,
    .release            = mt6620_ampc_release,
};





/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
BOOLEAN
glRegisterAmpc (
    IN P_GLUE_INFO_T prGlueInfo
    )
{
    ASSERT(prGlueInfo);

    if(prGlueInfo->rBowInfo.fgIsRegistered == TRUE) {
        return FALSE;
    }
    else {
#if 0
        // 1. allocate major number dynamically

    if(alloc_chrdev_region(&(prGlueInfo->rBowInfo.u4DeviceNumber),
                    0,  // first minor number
                    1,  // number
                    GLUE_BOW_DEVICE_NAME) !=0)

            return FALSE;
#endif

#if 1

#if defined (CONFIG_AMPC_CDEV_NUM)
    prGlueInfo->rBowInfo.u4DeviceNumber = MKDEV(CONFIG_AMPC_CDEV_NUM, 0);
#else
    prGlueInfo->rBowInfo.u4DeviceNumber = MKDEV(226, 0);
#endif

    if(register_chrdev_region(prGlueInfo->rBowInfo.u4DeviceNumber,
                    1,  // number
                    GLUE_BOW_DEVICE_NAME) !=0)

            return FALSE;
#endif

        // 2. spin-lock initialization
 //       spin_lock_init(&(prGlueInfo->rBowInfo.rSpinLock));

        // 3. initialize kfifo
            if ((kfifo_alloc((struct kfifo *) &(prGlueInfo->rBowInfo.rKfifo), GLUE_BOW_KFIFO_DEPTH, GFP_KERNEL)))
                goto fail_kfifo_alloc;

//        if(prGlueInfo->rBowInfo.prKfifo == NULL)
        if(&(prGlueInfo->rBowInfo.rKfifo) == NULL)
            goto fail_kfifo_alloc;

        // 4. initialize cdev
        cdev_init(&(prGlueInfo->rBowInfo.cdev), &mt6620_ampc_fops);
       // prGlueInfo->rBowInfo.cdev.owner = THIS_MODULE;
        prGlueInfo->rBowInfo.cdev.ops = &mt6620_ampc_fops;

        // 5. add character device
        if(cdev_add(&(prGlueInfo->rBowInfo.cdev),
                    prGlueInfo->rBowInfo.u4DeviceNumber,
                    1))
            goto fail_cdev_add;


        // 6. in queue initialization
        init_waitqueue_head(&(prGlueInfo->rBowInfo.outq));

        // 7. finish
        prGlueInfo->rBowInfo.fgIsRegistered = TRUE;
        return TRUE;

fail_cdev_add:
            kfifo_free(&(prGlueInfo->rBowInfo.rKfifo));
//        kfifo_free(prGlueInfo->rBowInfo.prKfifo);
fail_kfifo_alloc:
        unregister_chrdev_region(prGlueInfo->rBowInfo.u4DeviceNumber, 1);
        return FALSE;
    }
} /* end of glRegisterAmpc */


/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
BOOLEAN
glUnregisterAmpc (
    IN P_GLUE_INFO_T prGlueInfo
    )
{
    ASSERT(prGlueInfo);

    if(prGlueInfo->rBowInfo.fgIsRegistered == FALSE) {
        return FALSE;
    }
    else {
        prGlueInfo->rBowInfo.fgIsRegistered = FALSE;

        // 1. free netdev if necessary
#if CFG_BOW_SEPARATE_DATA_PATH
        kalUninitBowDevice(prGlueInfo);
#endif

        // 2. removal of character device
        cdev_del(&(prGlueInfo->rBowInfo.cdev));

        // 3. free kfifo
//        kfifo_free(prGlueInfo->rBowInfo.prKfifo);
        kfifo_free(&(prGlueInfo->rBowInfo.rKfifo));
//        prGlueInfo->rBowInfo.prKfifo = NULL;
//        prGlueInfo->rBowInfo.rKfifo = NULL;

        // 4. free device number
        unregister_chrdev_region(prGlueInfo->rBowInfo.u4DeviceNumber, 1);

        return TRUE;
    }
} /* end of glUnregisterAmpc */


/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
static ssize_t
mt6620_ampc_read(
    IN struct file *filp,
    IN char __user *buf,
    IN size_t size,
    IN OUT loff_t *ppos)
{
    UINT_8 aucBuffer[MAX_BUFFER_SIZE];
    ssize_t retval;

    P_GLUE_INFO_T prGlueInfo;
    prGlueInfo = (P_GLUE_INFO_T)(filp->private_data);

    ASSERT(prGlueInfo);

    if ((prGlueInfo->rBowInfo.fgIsRegistered == FALSE) || (prGlueInfo->u4Flag & GLUE_FLAG_HALT)) {
        return -EFAULT;
    }

    // size check
//    if(kfifo_len(prGlueInfo->rBowInfo.prKfifo) >= size)
    if(kfifo_len(&(prGlueInfo->rBowInfo.rKfifo)) >= size)
        retval = size;
    else
        retval = kfifo_len(&(prGlueInfo->rBowInfo.rKfifo));
//        retval = kfifo_len(prGlueInfo->rBowInfo.prKfifo);

//    kfifo_get(prGlueInfo->rBowInfo.prKfifo, aucBuffer, retval);
//    kfifo_out(prGlueInfo->rBowInfo.prKfifo, aucBuffer, retval);
    if (!(kfifo_out(&(prGlueInfo->rBowInfo.rKfifo), aucBuffer, retval)))
        retval = -EIO;

    if(copy_to_user(buf, aucBuffer, retval))
        retval = -EIO;

    return retval;
}


/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
static ssize_t
mt6620_ampc_write(
    IN struct file *filp,
    OUT const char __user *buf,
    IN size_t size,
    IN OUT loff_t *ppos)
{
#if CFG_BOW_TEST
    UINT_8 i;
#endif

    UINT_8 aucBuffer[MAX_BUFFER_SIZE];
    P_AMPC_COMMAND prCmd;
    P_GLUE_INFO_T prGlueInfo;

    prGlueInfo = (P_GLUE_INFO_T)(filp->private_data);
    ASSERT(prGlueInfo);

    if ((prGlueInfo->rBowInfo.fgIsRegistered == FALSE) || (prGlueInfo->u4Flag & GLUE_FLAG_HALT)) {
        return -EFAULT;
    }

    if(size > MAX_BUFFER_SIZE)
        return -EINVAL;
    else if(copy_from_user(aucBuffer, buf, size))
        return -EIO;

#if CFG_BOW_TEST
    DBGLOG(BOW, EVENT, ("AMP driver CMD buffer size : %d.\n", size));

    for(i = 0; i < MAX_BUFFER_SIZE; i++)
    {
        DBGLOG(BOW, EVENT, ("AMP write content : 0x%x.\n", aucBuffer[i]));
    }

    DBGLOG(BOW, EVENT, ("BoW CMD write.\n"));
#endif

    prCmd = (P_AMPC_COMMAND) aucBuffer;

 #if CFG_BOW_TEST
    DBGLOG(BOW, EVENT, ("AMP write content payload length : %d.\n", prCmd->rHeader.u2PayloadLength));

    DBGLOG(BOW, EVENT, ("AMP write content header length : %d.\n", sizeof(AMPC_COMMAND_HEADER_T)));
 #endif

    // size check
    if(prCmd->rHeader.u2PayloadLength + sizeof(AMPC_COMMAND_HEADER_T) != size)
    {
  #if CFG_BOW_TEST
        DBGLOG(BOW, EVENT, ("Wrong CMD total length.\n"));
  #endif

        return -EINVAL;
    }

    if(wlanbowHandleCommand(prGlueInfo->prAdapter, prCmd) == WLAN_STATUS_SUCCESS)
        return size;
    else
        return -EINVAL;
}


/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
static long
mt6620_ampc_ioctl(
    IN struct file *filp,
    IN unsigned int cmd,
    IN OUT unsigned long arg)
{
    int err = 0;
    P_GLUE_INFO_T prGlueInfo;
    prGlueInfo = (P_GLUE_INFO_T)(filp->private_data);

    ASSERT(prGlueInfo);

    if ((prGlueInfo->rBowInfo.fgIsRegistered == FALSE) || (prGlueInfo->u4Flag & GLUE_FLAG_HALT)) {
        return -EFAULT;
    }

    // permission check
    if(_IOC_DIR(cmd) & _IOC_READ)
        err = !access_ok(VERIFY_WRITE, (void __user *)arg, _IOC_SIZE(cmd));
    else if (_IOC_DIR(cmd) & _IOC_WRITE)
        err =  !access_ok(VERIFY_READ, (void __user *)arg, _IOC_SIZE(cmd));
    if (err)
        return -EFAULT;

    // no ioctl is implemented yet
    return 0;
}


/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
static unsigned int
mt6620_ampc_poll(
    IN struct file *filp,
    IN poll_table *wait)
{
    unsigned int retval;
    P_GLUE_INFO_T prGlueInfo;
    prGlueInfo = (P_GLUE_INFO_T)(filp->private_data);

    ASSERT(prGlueInfo);

    if ((prGlueInfo->rBowInfo.fgIsRegistered == FALSE) || (prGlueInfo->u4Flag & GLUE_FLAG_HALT)) {
        return -EFAULT;
    }

    poll_wait(filp, &prGlueInfo->rBowInfo.outq, wait);

    retval = (POLLOUT | POLLWRNORM); // always accepts incoming command packets

//    DBGLOG(BOW, EVENT, ("mt6620_ampc_pol, POLLOUT | POLLWRNORM, %x\n", retval));

//    if(kfifo_len(prGlueInfo->rBowInfo.prKfifo) > 0)
    if(kfifo_len(&(prGlueInfo->rBowInfo.rKfifo)) > 0)
    {
        retval |= (POLLIN | POLLRDNORM);

//        DBGLOG(BOW, EVENT, ("mt6620_ampc_pol, POLLIN | POLLRDNORM, %x\n", retval));

    }

    return retval;
}


/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
static int
mt6620_ampc_open(
    IN struct inode *inodep,
    IN struct file *filp)
{
     P_GLUE_INFO_T  prGlueInfo;
     P_GL_BOW_INFO  prBowInfo;

     prBowInfo = container_of(inodep->i_cdev, GL_BOW_INFO, cdev);
     ASSERT(prBowInfo);

     prGlueInfo = container_of(prBowInfo, GLUE_INFO_T, rBowInfo);
     ASSERT(prGlueInfo);

     // set-up private data
     filp->private_data = prGlueInfo;

     return 0;
}


/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
static int
mt6620_ampc_release(
    IN struct inode *inodep,
    IN struct file *filp)
{
    P_GLUE_INFO_T prGlueInfo;
    prGlueInfo = (P_GLUE_INFO_T)(filp->private_data);

    ASSERT(prGlueInfo);

    return 0;
}


/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
VOID
kalIndicateBOWEvent(
    IN P_GLUE_INFO_T        prGlueInfo,
    IN P_AMPC_EVENT prEvent
    )
{
    size_t u4AvailSize, u4EventSize;

    ASSERT(prGlueInfo);
    ASSERT(prEvent);

    // check device
    if ((prGlueInfo->rBowInfo.fgIsRegistered == FALSE) || (prGlueInfo->u4Flag & GLUE_FLAG_HALT)) {
        return;
    }


    u4AvailSize =
        GLUE_BOW_KFIFO_DEPTH - kfifo_len(&(prGlueInfo->rBowInfo.rKfifo));


    u4EventSize =
        prEvent->rHeader.u2PayloadLength + sizeof(AMPC_EVENT_HEADER_T);

    // check kfifo availability
    if(u4AvailSize < u4EventSize) {
        DBGLOG(BOW, EVENT, ("[bow] no space for event: %d/%d\n",
                u4EventSize,
                u4AvailSize));
        return;
    }

    // queue into kfifo
//    kfifo_put(prGlueInfo->rBowInfo.prKfifo, (PUINT_8)prEvent, u4EventSize);
//    kfifo_in(prGlueInfo->rBowInfo.prKfifo, (PUINT_8)prEvent, u4EventSize);
    kfifo_in(&(prGlueInfo->rBowInfo.rKfifo), (PUINT_8)prEvent, u4EventSize);
    wake_up_interruptible(&(prGlueInfo->rBowInfo.outq));

    return;
}

/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
ENUM_BOW_DEVICE_STATE
kalGetBowState (
    IN P_GLUE_INFO_T        prGlueInfo,
    IN UINT_8                     aucPeerAddress[6]
    )
{
    UINT_8 i;

    ASSERT(prGlueInfo);

#if CFG_BOW_TEST
    DBGLOG(BOW, EVENT, ("kalGetBowState.\n"));
#endif

    for(i = 0 ; i < CFG_BOW_PHYSICAL_LINK_NUM ; i++)
    {
        if(EQUAL_MAC_ADDR(prGlueInfo->rBowInfo.arPeerAddr, aucPeerAddress) == 0)
        {

#if CFG_BOW_TEST
    DBGLOG(BOW, EVENT, ("kalGetBowState, aucPeerAddress %x, %x:%x:%x:%x:%x:%x.\n", i,
        aucPeerAddress[0],
        aucPeerAddress[1],
        aucPeerAddress[2],
        aucPeerAddress[3],
        aucPeerAddress[4],
        aucPeerAddress[5]));

    DBGLOG(BOW, EVENT, ("kalGetBowState, prGlueInfo->rBowInfo.aeState %x, %x.\n", i, prGlueInfo->rBowInfo.aeState[i]));

#endif

            return prGlueInfo->rBowInfo.aeState[i];
        }
    }

    return BOW_DEVICE_STATE_DISCONNECTED;
}


/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
BOOLEAN
kalSetBowState (
    IN P_GLUE_INFO_T            prGlueInfo,
    IN ENUM_BOW_DEVICE_STATE    eBowState,
    IN UINT_8                                 aucPeerAddress[6]
    )
{
    UINT_8 i;

    ASSERT(prGlueInfo);

#if CFG_BOW_TEST
    DBGLOG(BOW, EVENT, ("kalSetBowState.\n"));

    DBGLOG(BOW, EVENT, ("kalSetBowState, prGlueInfo->rBowInfo.arPeerAddr, %x:%x:%x:%x:%x:%x.\n",
        prGlueInfo->rBowInfo.arPeerAddr[0],
        prGlueInfo->rBowInfo.arPeerAddr[1],
        prGlueInfo->rBowInfo.arPeerAddr[2],
        prGlueInfo->rBowInfo.arPeerAddr[3],
        prGlueInfo->rBowInfo.arPeerAddr[4],
        prGlueInfo->rBowInfo.arPeerAddr[5]));

    DBGLOG(BOW, EVENT, ("kalSetBowState, aucPeerAddress, %x:%x:%x:%x:%x:%x.\n",
        aucPeerAddress[0],
        aucPeerAddress[1],
        aucPeerAddress[2],
        aucPeerAddress[3],
        aucPeerAddress[4],
        aucPeerAddress[5]));
#endif

    for(i = 0 ; i < CFG_BOW_PHYSICAL_LINK_NUM ; i++)
    {
        if(EQUAL_MAC_ADDR(prGlueInfo->rBowInfo.arPeerAddr, aucPeerAddress) == 0)
        {
            prGlueInfo->rBowInfo.aeState[i] = eBowState;

#if CFG_BOW_TEST
    DBGLOG(BOW, EVENT, ("kalSetBowState, aucPeerAddress %x, %x:%x:%x:%x:%x:%x.\n", i,
        aucPeerAddress[0],
        aucPeerAddress[1],
        aucPeerAddress[2],
        aucPeerAddress[3],
        aucPeerAddress[4],
        aucPeerAddress[5]));

    DBGLOG(BOW, EVENT, ("kalSetBowState, prGlueInfo->rBowInfo.aeState %x, %x.\n", i, prGlueInfo->rBowInfo.aeState[i]));
#endif

            return TRUE;
        }
    }

    return FALSE;
}


/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
ENUM_BOW_DEVICE_STATE
kalGetBowGlobalState (
    IN P_GLUE_INFO_T    prGlueInfo
    )
{
    UINT_32 i;

    ASSERT(prGlueInfo);


//Henry, can reduce this logic to indentify state change

    for(i = 0 ; i < CFG_BOW_PHYSICAL_LINK_NUM ; i++) {
        if(prGlueInfo->rBowInfo.aeState[i] == BOW_DEVICE_STATE_CONNECTED) {
            return BOW_DEVICE_STATE_CONNECTED;
        }
    }

    for(i = 0 ; i < CFG_BOW_PHYSICAL_LINK_NUM ; i++) {
        if(prGlueInfo->rBowInfo.aeState[i] == BOW_DEVICE_STATE_STARTING) {
            return BOW_DEVICE_STATE_STARTING;
        }
    }

    return BOW_DEVICE_STATE_DISCONNECTED;
}

/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
UINT_32
kalGetBowFreqInKHz(
    IN P_GLUE_INFO_T            prGlueInfo
    )
{
    ASSERT(prGlueInfo);

    return prGlueInfo->rBowInfo.u4FreqInKHz;
}


/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
UINT_8
kalGetBowRole(
    IN P_GLUE_INFO_T        prGlueInfo,
    IN PARAM_MAC_ADDRESS    rPeerAddr
    )
{
    UINT_32 i;

    ASSERT(prGlueInfo);

    for(i = 0 ; i < CFG_BOW_PHYSICAL_LINK_NUM ; i++) {
        if(EQUAL_MAC_ADDR(prGlueInfo->rBowInfo.arPeerAddr[i], rPeerAddr) == 0) {
            return prGlueInfo->rBowInfo.aucRole[i];
        }
    }

    return 0;
}


/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
VOID
kalSetBowRole(
    IN P_GLUE_INFO_T        prGlueInfo,
    IN UINT_8               ucRole,
    IN PARAM_MAC_ADDRESS    rPeerAddr
    )
{
    UINT_32 i;

    ASSERT(prGlueInfo);
    ASSERT(ucRole <= 1);

    for(i = 0 ; i < CFG_BOW_PHYSICAL_LINK_NUM ; i++) {
        if(EQUAL_MAC_ADDR(prGlueInfo->rBowInfo.arPeerAddr[i], rPeerAddr) == 0) {
            prGlueInfo->rBowInfo.aucRole[i] = ucRole; //Henry, 0 : Responder, 1 : Initiator
        }
    }
}


/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
UINT_8
kalGetBowAvailablePhysicalLinkCount(
    IN P_GLUE_INFO_T        prGlueInfo
    )
{
    UINT_8 i;
    UINT_8 ucLinkCount = 0;

    ASSERT(prGlueInfo);

    for(i = 0 ; i < CFG_BOW_PHYSICAL_LINK_NUM ; i++) {
        if(prGlueInfo->rBowInfo.aeState[i] == BOW_DEVICE_STATE_DISCONNECTED) {
            ucLinkCount++;
        }
    }

#if 0//CFG_BOW_TEST
    DBGLOG(BOW, EVENT, ("kalGetBowAvailablePhysicalLinkCount, ucLinkCount, %c.\n", ucLinkCount));
#endif

    return ucLinkCount;
}

#if CFG_BOW_SEPARATE_DATA_PATH

/* Net Device Hooks */
/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
static int
bowOpen(
    IN struct net_device *prDev
    )
{
    P_GLUE_INFO_T prGlueInfo = NULL;
    P_ADAPTER_T prAdapter = NULL;

    ASSERT(prDev);

    prGlueInfo = *((P_GLUE_INFO_T *) netdev_priv(prDev));
    ASSERT(prGlueInfo);

    prAdapter = prGlueInfo->prAdapter;
    ASSERT(prAdapter);

    /* 2. carrier on & start TX queue */
    netif_carrier_on(prDev);
    netif_tx_start_all_queues(prDev);

    return 0; /* success */
}


/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
static int
bowStop(
    IN struct net_device *prDev
    )
{
    P_GLUE_INFO_T prGlueInfo = NULL;
    P_ADAPTER_T prAdapter = NULL;

    ASSERT(prDev);

    prGlueInfo = *((P_GLUE_INFO_T *) netdev_priv(prDev));
    ASSERT(prGlueInfo);

    prAdapter = prGlueInfo->prAdapter;
    ASSERT(prAdapter);

    /* 1. stop TX queue */
    netif_tx_stop_all_queues(prDev);

    /* 2. turn of carrier */
    if(netif_carrier_ok(prDev)) {
        netif_carrier_off(prDev);
    }

    return 0;
};


/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
static int
bowHardStartXmit(
    IN struct sk_buff *prSkb,
    IN struct net_device *prDev
    )
{
    P_GLUE_INFO_T prGlueInfo = *((P_GLUE_INFO_T *) netdev_priv(prDev));

    P_QUE_ENTRY_T prQueueEntry = NULL;
    P_QUE_T prTxQueue = NULL;
    UINT_16 u2QueueIdx = 0;
    UINT_8 ucDSAP, ucSSAP, ucControl;
    UINT_8 aucOUI[3];
    PUINT_8 aucLookAheadBuf = NULL;

#if CFG_BOW_TEST
    UINT_32 i;
#endif

    GLUE_SPIN_LOCK_DECLARATION();

    ASSERT(prSkb);
    ASSERT(prDev);
    ASSERT(prGlueInfo);

    aucLookAheadBuf = prSkb->data;

    ucDSAP = *(PUINT_8) &aucLookAheadBuf[ETH_LLC_OFFSET];
    ucSSAP = *(PUINT_8) &aucLookAheadBuf[ETH_LLC_OFFSET + 1];
    ucControl = *(PUINT_8) &aucLookAheadBuf[ETH_LLC_OFFSET + 2];
    aucOUI[0] = *(PUINT_8) &aucLookAheadBuf[ETH_SNAP_OFFSET];
    aucOUI[1] = *(PUINT_8) &aucLookAheadBuf[ETH_SNAP_OFFSET + 1];
    aucOUI[2] = *(PUINT_8) &aucLookAheadBuf[ETH_SNAP_OFFSET + 2];

    if (!(ucDSAP == ETH_LLC_DSAP_SNAP &&
            ucSSAP == ETH_LLC_SSAP_SNAP &&
            ucControl == ETH_LLC_CONTROL_UNNUMBERED_INFORMATION &&
            aucOUI[0] == ETH_SNAP_BT_SIG_OUI_0 &&
            aucOUI[1] == ETH_SNAP_BT_SIG_OUI_1 &&
            aucOUI[2] == ETH_SNAP_BT_SIG_OUI_2) || (prSkb->len > 1514))
    {

#if CFG_BOW_TEST
        DBGLOG(BOW, TRACE, ("Invalid BOW packet, skip tx\n"));
#endif

        dev_kfree_skb(prSkb);
        return NETDEV_TX_OK;
     }

    if (prGlueInfo->u4Flag & GLUE_FLAG_HALT) {
        DBGLOG(BOW, TRACE, ("GLUE_FLAG_HALT skip tx\n"));
        dev_kfree_skb(prSkb);
        return NETDEV_TX_OK;
    }

    prQueueEntry = (P_QUE_ENTRY_T) GLUE_GET_PKT_QUEUE_ENTRY(prSkb);
    prTxQueue = &prGlueInfo->rTxQueue;

#if CFG_BOW_TEST
    DBGLOG(BOW, TRACE, ("Tx sk_buff->len: %d\n", prSkb->len));
    DBGLOG(BOW, TRACE, ("Tx sk_buff->data_len: %d\n", prSkb->data_len));
    DBGLOG(BOW, TRACE, ("Tx sk_buff->data:\n"));

    for(i = 0; i < prSkb->len; i++)
    {
        DBGLOG(BOW, TRACE, ("%4x", prSkb->data[i]));

        if((i+1)%16 ==0)
        {
            DBGLOG(BOW, TRACE, ("\n"));
        }
    }

    DBGLOG(BOW, TRACE, ("\n");
#endif

#if CFG_BOW_TEST
//    g_u4CurrentSysTime = (OS_SYSTIME)kalGetTimeTick();

    g_u4CurrentSysTime = (OS_SYSTIME) jiffies_to_usecs(jiffies);

    i = g_u4CurrentSysTime - g_u4PrevSysTime;

    if ( (i >> 10) > 0)
    {
        i = 10;
    }
    else
    {
        i = i >> 7;
    }

    g_arBowRevPalPacketTime[i]++;

    g_u4PrevSysTime = g_u4CurrentSysTime;

#endif

    if (wlanProcessSecurityFrame(prGlueInfo->prAdapter, (P_NATIVE_PACKET) prSkb) == FALSE) {
    	GLUE_ACQUIRE_SPIN_LOCK(prGlueInfo, SPIN_LOCK_TX_QUE);
    	QUEUE_INSERT_TAIL(prTxQueue, prQueueEntry);
        GLUE_RELEASE_SPIN_LOCK(prGlueInfo, SPIN_LOCK_TX_QUE);


    	GLUE_INC_REF_CNT(prGlueInfo->i4TxPendingFrameNum);
    	GLUE_INC_REF_CNT(prGlueInfo->ai4TxPendingFrameNumPerQueue[NETWORK_TYPE_BOW_INDEX][u2QueueIdx]);

    	if (prGlueInfo->ai4TxPendingFrameNumPerQueue[NETWORK_TYPE_BOW_INDEX][u2QueueIdx] >= CFG_TX_STOP_NETIF_PER_QUEUE_THRESHOLD) {
            netif_stop_subqueue(prDev, u2QueueIdx);
    	}
    }
    else {
        GLUE_INC_REF_CNT(prGlueInfo->i4TxPendingSecurityFrameNum);
    }

    kalSetEvent(prGlueInfo);

    /* For Linux, we'll always return OK FLAG, because we'll free this skb by ourself */
    return NETDEV_TX_OK;
}


// callbacks for netdevice
static const struct net_device_ops bow_netdev_ops = {
    .ndo_open               = bowOpen,
    .ndo_stop               = bowStop,
    .ndo_start_xmit         = bowHardStartXmit,
};

/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
BOOLEAN
kalInitBowDevice(
    IN P_GLUE_INFO_T        prGlueInfo,
    IN const char           *prDevName
    )
{
    P_ADAPTER_T prAdapter;
    P_GL_HIF_INFO_T prHif;
    PARAM_MAC_ADDRESS rMacAddr;

    ASSERT(prGlueInfo);
    ASSERT(prGlueInfo->rBowInfo.fgIsRegistered == TRUE);

    prAdapter = prGlueInfo->prAdapter;
    ASSERT(prAdapter);

    prHif = &prGlueInfo->rHifInfo;
    ASSERT(prHif);

    if(prGlueInfo->rBowInfo.fgIsNetRegistered == FALSE) {
        prGlueInfo->rBowInfo.prDevHandler = alloc_netdev_mq(sizeof(P_GLUE_INFO_T), prDevName, ether_setup, CFG_MAX_TXQ_NUM);

        if (!prGlueInfo->rBowInfo.prDevHandler) {
            return FALSE;
        }
        else {
            /* 1. setup netdev */
            /* 1.1 Point to shared glue structure */
            *((P_GLUE_INFO_T *) netdev_priv(prGlueInfo->rBowInfo.prDevHandler)) = prGlueInfo;

            /* 1.2 fill hardware address */
            COPY_MAC_ADDR(rMacAddr, prAdapter->rMyMacAddr);
            rMacAddr[0] |= 0x2; // change to local administrated address
            memcpy(prGlueInfo->rBowInfo.prDevHandler->dev_addr, rMacAddr, ETH_ALEN);
            memcpy(prGlueInfo->rBowInfo.prDevHandler->perm_addr, prGlueInfo->rBowInfo.prDevHandler->dev_addr, ETH_ALEN);

            /* 1.3 register callback functions */
            prGlueInfo->rBowInfo.prDevHandler->netdev_ops = &bow_netdev_ops;

#if (MTK_WCN_HIF_SDIO == 0)
            SET_NETDEV_DEV(prGlueInfo->rBowInfo.prDevHandler, prHif->Dev);
#endif

            register_netdev(prGlueInfo->rBowInfo.prDevHandler);

            /* 2. net device initialize */
            netif_carrier_off(prGlueInfo->rBowInfo.prDevHandler);
            netif_tx_stop_all_queues(prGlueInfo->rBowInfo.prDevHandler);

            /* 3. finish */
            prGlueInfo->rBowInfo.fgIsNetRegistered = TRUE;
        }
    }

    return TRUE;
}


/*----------------------------------------------------------------------------*/
/*----------------------------------------------------------------------------*/
BOOLEAN
kalUninitBowDevice(
    IN P_GLUE_INFO_T        prGlueInfo
    )
{
    ASSERT(prGlueInfo);
    //ASSERT(prGlueInfo->rBowInfo.fgIsRegistered == TRUE);

    if(prGlueInfo->rBowInfo.fgIsNetRegistered == TRUE) {

        prGlueInfo->rBowInfo.fgIsNetRegistered = FALSE;

        if(netif_carrier_ok(prGlueInfo->rBowInfo.prDevHandler)) {
            netif_carrier_off(prGlueInfo->rBowInfo.prDevHandler);
        }

        netif_tx_stop_all_queues(prGlueInfo->rBowInfo.prDevHandler);

        /* netdevice unregistration & free */
        unregister_netdev(prGlueInfo->rBowInfo.prDevHandler);
        free_netdev(prGlueInfo->rBowInfo.prDevHandler);
        prGlueInfo->rBowInfo.prDevHandler = NULL;

        return TRUE;

    }
    else {
        return FALSE;
    }
}

#endif // CFG_BOW_SEPARATE_DATA_PATH
#endif // CFG_ENABLE_BT_OVER_WIFI

