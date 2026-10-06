/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * dvfel - differences of the Amlogic 5.4 GKI media API (Android, e.g. the
 * Dune HD Pro One 8K Plus firmware) against the 5.15 common_drivers API.
 */
#ifndef DVFEL_COMPAT_H
#define DVFEL_COMPAT_H

#include <linux/version.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 0)
#define DVFEL_KERNEL_54		1

/* VICP descriptors carry the older _t tags (identical layout) */
#define vicp_data_config_s	vicp_data_config_t
#define dma_data_config_s	dma_data_config_t

/*
 * 5.15 renamed omx_index to frame_index (same slot). dvfel reads it only
 * for the hardware decoded EL pairing, which the Android path never uses.
 */
#define frame_index		omx_index

/* exported by the 5.4 GKI aml_media (sets up the VICP RDMA buffers) */
int vicp_process_enable(int enable);
#endif

#endif /* DVFEL_COMPAT_H */
