/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef IRIS_SECURE_ARP_H
#define IRIS_SECURE_ARP_H

struct iris_core;
struct iris_inst;
struct iris_buffer;
struct platform_driver;
extern struct platform_driver iris_secure_arp_driver;
int iris_secure_arp_init(struct iris_core *core);
int iris_secure_arp_alloc(struct iris_inst *inst, struct iris_buffer *buffer);
void iris_secure_arp_free(struct iris_inst *inst, struct iris_buffer *buffer);
#endif
