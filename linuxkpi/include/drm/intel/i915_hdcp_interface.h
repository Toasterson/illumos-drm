/*
 * SPDX-License-Identifier: MIT
 * Copyright 2026 The illumos-drm authors
 *
 * Linux's <drm/intel/i915_hdcp_interface.h> (GPL-2.0+; independent
 * declarations), reduced to what i915's display types refer to: the port
 * descriptions i915 keeps per HDCP-capable connector and the interface to
 * the HDCP firmware (MEI/GSC). illumos-drm builds no HDCP: the HDCP TUs
 * are stubbed (drivers/i915/display-tus.txt) and no arbiter is ever
 * registered; the enumerator values are the firmware interface's.
 */
#ifndef _I915_HDCP_INTERFACE_H_
#define	_I915_HDCP_INTERFACE_H_

#include <linux/mutex.h>
#include <linux/device.h>
#include <drm/display/drm_hdcp.h>

enum hdcp_port_type {
	HDCP_PORT_TYPE_INVALID,
	HDCP_PORT_TYPE_INTEGRATED,
	HDCP_PORT_TYPE_LSPCON,
	HDCP_PORT_TYPE_CPDP
};

enum hdcp_wired_protocol {
	HDCP_PROTOCOL_INVALID,
	HDCP_PROTOCOL_HDMI,
	HDCP_PROTOCOL_DP
};

enum hdcp_ddi {
	HDCP_DDI_INVALID_PORT = 0x0,
	HDCP_DDI_B = 1,
	HDCP_DDI_C,
	HDCP_DDI_D,
	HDCP_DDI_E,
	HDCP_DDI_F,
	HDCP_DDI_A = 7,
	HDCP_DDI_RANGE_END = HDCP_DDI_A
};

enum hdcp_transcoder {
	HDCP_INVALID_TRANSCODER = 0x00,
	HDCP_TRANSCODER_EDP,
	HDCP_TRANSCODER_DSI0,
	HDCP_TRANSCODER_DSI1,
	HDCP_TRANSCODER_A = 0x10,
	HDCP_TRANSCODER_B,
	HDCP_TRANSCODER_C,
	HDCP_TRANSCODER_D
};

/* One HDCP port as the firmware sees it. */
struct hdcp_port_data {
	enum hdcp_ddi hdcp_ddi;
	enum hdcp_transcoder hdcp_transcoder;
	u8 port_type;		/* enum hdcp_port_type */
	u8 protocol;		/* enum hdcp_wired_protocol */
	u16 k;			/* number of streams */
	u32 seq_num_m;
	struct hdcp2_streamid_type *streams;
};

/* The firmware's HDCP 2.x operations (never provided on illumos). */
struct i915_hdcp_ops {
	struct lx_module *owner;
	int (*initiate_hdcp2_session)(struct device *dev,
	    struct hdcp_port_data *data, struct hdcp2_ake_init *ake_data);
	int (*verify_receiver_cert_prepare_km)(struct device *dev,
	    struct hdcp_port_data *data, struct hdcp2_ake_send_cert *rx_cert,
	    bool *km_stored, struct hdcp2_ake_no_stored_km *ek_pub_km,
	    size_t *msg_sz);
	int (*verify_hprime)(struct device *dev, struct hdcp_port_data *data,
	    struct hdcp2_ake_send_hprime *rx_hprime);
	int (*store_pairing_info)(struct device *dev,
	    struct hdcp_port_data *data,
	    struct hdcp2_ake_send_pairing_info *pairing_info);
	int (*initiate_locality_check)(struct device *dev,
	    struct hdcp_port_data *data, struct hdcp2_lc_init *lc_init_data);
	int (*verify_lprime)(struct device *dev, struct hdcp_port_data *data,
	    struct hdcp2_lc_send_lprime *rx_lprime);
	int (*get_session_key)(struct device *dev, struct hdcp_port_data *data,
	    struct hdcp2_ske_send_eks *ske_data);
	int (*repeater_check_flow_prepare_ack)(struct device *dev,
	    struct hdcp_port_data *data,
	    struct hdcp2_rep_send_receiverid_list *rep_topology,
	    struct hdcp2_rep_send_ack *rep_send_ack);
	int (*verify_mprime)(struct device *dev, struct hdcp_port_data *data,
	    struct hdcp2_rep_stream_ready *stream_ready);
	int (*enable_hdcp_authentication)(struct device *dev,
	    struct hdcp_port_data *data);
	int (*close_hdcp_session)(struct device *dev,
	    struct hdcp_port_data *data);
};

/* The binding of i915 to the firmware's HDCP interface. */
struct i915_hdcp_arbiter {
	struct device *hdcp_dev;
	const struct i915_hdcp_ops *ops;
	struct mutex mutex;
};

#endif
