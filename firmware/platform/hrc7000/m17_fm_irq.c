/* SPDX-License-Identifier: GPL-3.0-or-later */
/* FM-only interrupt bridge: M17 needs PCM bank events, not the DMR stack. */
#include "config.h"
#if CONFIG_M17
#include "dmr_glue.h"
#include "platform.h"
static hrc7000_t *modem;
static void (*rx_hook)(void),(*tx_hook)(void);
static void system_irq(void) {
 uint32_t bits=hrc7000_rd(modem,C7_SYS_INTERP_LIST)&hrc7000_rd(modem,C7_SYS_INTERP_MASK);
 if((bits&C7_SYS_FM_TX)&&tx_hook)tx_hook();
 if((bits&C7_SYS_FM_RX)&&rx_hook)rx_hook();
 hrc7000_wr(modem,C7_SYS_INTERP_CLEAR,bits);
}
void hrc_m17_fm_irq_init(hrc7000_t *m) {modem=m;hrc_irq_register(IRQ_MODEM_SYS,system_irq);}
bool hrc_dmr_fm_rx_hook(void (*fn)(void)) {uint32_t s=hrc_irq_save();rx_hook=fn;hrc_irq_enable(IRQ_MODEM_SYS,rx_hook||tx_hook);hrc_irq_restore(s);return true;}
bool hrc_dmr_fm_tx_hook(void (*fn)(void)) {uint32_t s=hrc_irq_save();tx_hook=fn;hrc_irq_enable(IRQ_MODEM_SYS,rx_hook||tx_hook);hrc_irq_restore(s);return true;}
void hrc_dmr_enter(void){}void hrc_dmr_leave(void){}void hrc_dmr_rx_restart(void){}
bool hrc_dmr_tx_active(void){return false;}void hrc_dmr_tx_drop(void){}
#endif
