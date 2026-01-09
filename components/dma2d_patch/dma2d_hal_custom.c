#include "hal/dma2d_hal.h"
#include "hal/dma2d_ll.h"

void __wrap_dma2d_hal_tx_reset_channel(dma2d_hal_context_t *hal, uint32_t channel)
{
    dma2d_ll_tx_abort(hal->dev, channel, true);
    for (int i = 0; !dma2d_ll_tx_is_reset_avail(hal->dev, channel) && i < 100; i++);
    dma2d_ll_tx_reset_channel(hal->dev, channel);
    dma2d_ll_tx_abort(hal->dev, channel, false);
}
