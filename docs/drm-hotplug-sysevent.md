# illumos DRM connector notifications (pilot ABI version 1)

The carried KMS module maps drm_sysfs_hotplug_event, connector hotplug and
connector property events to `ddi_log_sysevent`. Class `EC_drm`, subclass
`ESC_drm_hotplug`, vendor `illumos-drm`. Connector registration/removal also
marks topology dirty. The Linux helper's existing client notifications remain
unchanged. This producer covers the carried Linux KMS path (i915n); native Rust
KMS drivers must explicitly integrate a producer or use userland polling.

| nvlist name | Type | Meaning |
| --- | --- | --- |
| version | uint32 | 1; reject unknown formats |
| driver-major | uint32 | DDI driver major |
| primary-minor | uint32 | Primary DRM node minor before clone-open encoding |
| device-path | string | ddi_pathname path, for diagnostics |
| connector-id | uint32 | DRM object ID; 0 means full/merged topology |
| reasons | uint32 | OR of HPD=1, topology=2, property=4 |
| sequence | uint64 | Per-registration emission-attempt sequence, starts at 1 |

Events are hints to query current state, not a lossless connector history.
Sequence resets at registration and gaps are allowed. Connector IDs can be
reused; consumers must never cache a pointer or infer current presence from an
event alone. The Xorg consumer always reconciles the full resource list, using
its existing MST path and RRGetInfo, including disappearance and later creation.
Zaphod's upstream CRTC-count restriction remains; this does not add MST support
to Zaphod configurations.

A per-device entry is inserted before modeset object registration. Notifications
only update IDs/reasons and queue a delayed work item under the registry lock;
they allocate nothing and do not call syseventd. The first deadline is 250 ms,
is not extended by further notifications, and subsequent emissions are limited
to one per second per device. Differing IDs collapse to zero. A worker builds
an nvlist and calls ddi_log_sysevent with DDI_NOSLEEP. Delivery failures get at
most three retries without a new event. `drm_hotplug_sent`, `drm_hotplug_failed`
and `drm_hotplug_coalesced` are mdb-visible counters, serialized by that lock.

Unregister removes the entry under the same lock used for notification lookup,
then synchronously cancels/drains delayed work before connector teardown and
freeing the entry. A running worker can finish while unregister waits; it cannot
requeue after the stop flag. Registration failures drain the entry too. This
avoids changing the facade's device layout or retaining connector pointers.
The driver/core must still honor its normal device-lifetime contract.

Xorg's modesetting patch binds one libsysevent subscriber for all its screens.
The callback thread validates the version and matches driver major plus primary
minor, writes only to a nonblocking pipe, and never calls Xorg/DRM/RandR. Facade
clone-open minors encode the primary node in the low nine bits; the consumer
checks the DRM driver name before applying that convention. On the server
thread, pipe notifications coalesce into a timer and invoke the same reprobe as
udev. Events received off-VT remain pending; existing EnterVT also re-probes.
Unbind joins the delivery thread. Screen removal first removes its pipe entry
under the callback mutex, then drains/unbinds before closing/freeing resources.

No new daemon or SMF service is needed: the existing system/sysevent service is
the transport. A failed subscription leaves the screen usable. The separate
polling patch provides both a fallback and a low-frequency lost-event check;
its HotplugPoll option is independent of event subscription.

Current i915n DDI glue rejects suspend/resume. This bridge adds neither power
management nor a false suspend claim. VT/re-registration paths rescan; real
resume integration must issue a full topology notification after the driver's
future hardware resume completes. Test physical HPD, MST docks, cable storms,
teardown and suspend only on hardware implementing those paths.
