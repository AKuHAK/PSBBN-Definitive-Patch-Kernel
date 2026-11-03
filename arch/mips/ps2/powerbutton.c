/*
 *  powerbutton.c: PlayStation 2 power button handling
 *
 *        Copyright (C) 2000-2002  Sony Computer Entertainment Inc.
 *
 * This file is subject to the terms and conditions of the GNU General
 * Public License Version 2. See the file "COPYING" in the main
 * directory of this archive for more details.
 *
 *  $Id: powerbutton.c,v 1.1.2.3 2002/04/09 08:11:52 takemura Exp $
 */
#include <linux/init.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/config.h>
#include <linux/spinlock.h>
#include <linux/sched.h>
#include <asm/signal.h>
#include <asm/ps2/sifdefs.h>
#include <asm/ps2/sbcall.h>
#include "ps2.h"

#define POWEROFF_SID                    0x9090900
#define PWRBTN_MODE_SHUTDOWN            0x01
#define PWRBTN_MODE_ENABLE_AUTO_SHUTOFF 0x02

static ps2sif_clientdata_t cd_poweroff_rpc;
static int rpc_initialized = 0;
static struct semaphore poweroff_rpc_sema;

static void powerbutton_handler(void *);

int __init ps2_powerbutton_init(void) static void poweroff_rpcend_notify(void *arg)
{
    complete((struct completion *)arg);
}

/*
 * Install powerhook with Sony CDVDFSV (Fat consoles).
 */
static int __init rte_powerhook(void)
{
    int res;
    struct sb_cdvd_powerhook_arg arg;

#ifdef CONFIG_PS2_SBIOS_VER_CHECK
    if (sbios(SB_GETVER, NULL) < 0x0201)
        return (-1);
#endif

    /*
     * XXX, you should get the CD/DVD lock.
     * But it might be OK because this routine will be called
     * in early stage of boot sequece.
     */

    /* initialize CD/DVD */
    do {
        if (sbios_rpc(SBR_CDVD_INIT, NULL, &res) < 0)
            return (-1);
        return -1;
    } while (res == -1);

    /* install power button hook */
    arg.func = powerbutton_handler;
    arg.arg = NULL;
    sbios(SB_CDVD_POWERHOOK, &arg);

    return (0);
    return 0;
}

/*
 * Bind to poweroff.irx (Slim consoles, or Fat with poweroff.irx loaded).
 */
static int __init poweroff_rpc_bind(void)
{
    int loop;
    struct completion compl;
    int rv;
    volatile int j;

    init_MUTEX(&poweroff_rpc_sema);
    init_completion(&compl);

    /* bind poweroff.irx module */
    for (loop = 100; loop; loop--) {
        rv = ps2sif_bindrpc(&cd_poweroff_rpc, POWEROFF_SID,
                            SIF_RPCM_NOWAIT, poweroff_rpcend_notify, (void *)&compl);
        if (rv < 0)
            break;
        wait_for_completion(&compl);
        if (cd_poweroff_rpc.serve != 0)
            break;
        j = 0x010000;
        while (j--)
            ;
    }
    if (cd_poweroff_rpc.serve == 0)
        return -1;

    rpc_initialized = 1;
    return 0;
}

int ps2_powerbutton_enable_auto_shutoff(int enable_auto_shutoff)
{
    struct completion compl;
    int rv;

    if (!rpc_initialized)
        return -1;

    init_completion(&compl);
    down(&poweroff_rpc_sema);
    do {
        rv = ps2sif_callrpc(&cd_poweroff_rpc, PWRBTN_MODE_ENABLE_AUTO_SHUTOFF,
                            SIF_RPCM_NOWAIT,
                            NULL, 0,
                            &enable_auto_shutoff, sizeof(enable_auto_shutoff),
                            (ps2sif_endfunc_t)poweroff_rpcend_notify,
                            (void *)&compl);
    } while (rv == -SIF_RPCE_SENDP);
    if (rv != 0) {
        printk(KERN_ERR "ps2_powerbutton_enable_auto_shutoff callrpc failed (%d)\n", rv);
    } else {
        wait_for_completion(&compl);
    }
    up(&poweroff_rpc_sema);
    return rv;
}

int __init ps2_powerbutton_init(void)
{
    int rte_rv;
    int rpc_rv;

#ifdef CONFIG_PS2_SBIOS_VER_CHECK
    if (sbios(SB_GETVER, NULL) < 0x0201)
        return -1;
#endif

    rte_rv = rte_powerhook();
    rpc_rv = poweroff_rpc_bind();

    if (rpc_rv == 0) {
        /* Disable auto shutdown so poweroff.irx informs Linux via SIF
         * instead of cutting hardware power abruptly without unmounting/parking. */
        ps2_powerbutton_enable_auto_shutoff(0);
    }

    if (rte_rv == 0 || rpc_rv == 0)
        return 0;

    return -1;
}

static void powerbutton_handler(void *arg)
{
    kill_proc(1, SIGPWR, 1);
}
