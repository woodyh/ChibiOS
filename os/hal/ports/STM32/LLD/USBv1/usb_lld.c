/*
    ChibiOS - Copyright (C) 2006..2015 Giovanni Di Sirio

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

/**
 * @file    STM32/USBv1/usb_lld.c
 * @brief   STM32 USB subsystem low level driver source.
 *
 * @addtogroup USB
 * @{
 */

#include <string.h>

#include "hal.h"

#if HAL_USE_USB || defined(__DOXYGEN__)

/*===========================================================================*/
/* Driver local definitions.                                                 */
/*===========================================================================*/

#define BTABLE_ADDR     0x0000

#define EPR_EP_TYPE_IS_ISO(epr) ((epr & EPR_EP_TYPE_MASK) == EPR_EP_TYPE_ISO)

/*===========================================================================*/
/* Driver exported variables.                                                */
/*===========================================================================*/

/** @brief USB1 driver identifier.*/
#if STM32_USB_USE_USB1 || defined(__DOXYGEN__)
USBDriver USBD1;
#endif

/*===========================================================================*/
/* Driver local variables and types.                                         */
/*===========================================================================*/

/**
 * @brief   EP0 state.
 * @note    It is an union because IN and OUT endpoints are never used at the
 *          same time for EP0.
 */
static union {
  /**
   * @brief   IN EP0 state.
   */
  USBInEndpointState in;
  /**
   * @brief   OUT EP0 state.
   */
  USBOutEndpointState out;
} ep0_state;

/**
 * @brief   Buffer for the EP0 setup packets.
 */
static uint8_t ep0setup_buffer[8];

/**
 * @brief   EP0 initialization structure.
 */
static const USBEndpointConfig ep0config = {
  USB_EP_MODE_TYPE_CTRL,
  _usb_ep0setup,
  _usb_ep0in,
  _usb_ep0out,
  0x40,
  0x40,
  &ep0_state.in,
  &ep0_state.out,
  1,
  ep0setup_buffer
};

/*===========================================================================*/
/* Driver local functions.                                                   */
/*===========================================================================*/

/**
 * @brief   Resets the packet memory allocator.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 */
static void usb_pm_reset(USBDriver *usbp) {

  /* The first 64 bytes are reserved for the descriptors table. The effective
     available RAM for endpoint buffers is just 448 bytes.*/
  usbp->pmnext = 64;
}

/**
 * @brief   Resets the packet memory allocator.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 * @param[in] size      size of the packet buffer to allocate
 */
static uint32_t usb_pm_alloc(USBDriver *usbp, size_t size) {
  uint32_t next;

  next = usbp->pmnext;
  usbp->pmnext += (size + 1) & ~1;
  osalDbgAssert(usbp->pmnext <= STM32_USB_PMA_SIZE, "PMA overflow");
  return next;
}

/**
 * @brief   Reads from a dedicated packet buffer.
 *
 * @param[in] udp       pointer to a @p stm32_usb_descriptor_t
 * @param[out] buf      buffer where to copy the packet data
 * @param[in] n         maximum number of bytes to copy. This value must
 *                      not exceed the maximum packet size for this endpoint.
 *
 * @notapi
 */
static void usb_packet_read_to_buffer(stm32_usb_descriptor_t *udp,
                                      uint8_t *buf, size_t n) {
  stm32_usb_pma_t *pmap;
  uint32_t w;
  size_t i;

  pmap = USB_ADDR2PTR(udp->RXADDR0);

  i = 0;
  w = 0; /* Useless but silences a warning.*/
  while (i < n) {
    if ((i & 1) == 0){
      w = *pmap;
      *buf = (uint8_t)w;
      pmap++;
    }
    else {
      *buf = (uint8_t)(w >> 8);
    }
    i++;
    buf++;
  }
}

/**
 * @brief   Reads from a dedicated packet buffer.
 *
 * @param[in] udp       pointer to a @p stm32_usb_descriptor_t
 * @param[in] iqp       pointer to an @p input_queue_t object
 * @param[in] n         maximum number of bytes to copy. This value must
 *                      not exceed the maximum packet size for this endpoint.
 *
 * @notapi
 */
static void usb_packet_read_to_queue(stm32_usb_descriptor_t *udp,
                                     input_queue_t *iqp, size_t n) {
  size_t nhw;
  stm32_usb_pma_t *pmap= USB_ADDR2PTR(udp->RXADDR0);

  nhw = n / 2;
  while (nhw > 0) {
    stm32_usb_pma_t w;

    w = *pmap++;
    *iqp->q_wrptr++ = (uint8_t)w;
    if (iqp->q_wrptr >= iqp->q_top)
      iqp->q_wrptr = iqp->q_buffer;
    *iqp->q_wrptr++ = (uint8_t)(w >> 8);
    if (iqp->q_wrptr >= iqp->q_top)
      iqp->q_wrptr = iqp->q_buffer;
    nhw--;
  }
  /* Last byte for odd numbers.*/
  if ((n & 1) != 0) {
    *iqp->q_wrptr++ = (uint8_t)*pmap;
    if (iqp->q_wrptr >= iqp->q_top)
      iqp->q_wrptr = iqp->q_buffer;
  }

  /* Updating queue.*/
  osalSysLockFromISR();

  iqp->q_counter += n;
  osalThreadDequeueAllI(&iqp->q_waiting, Q_OK);

  osalSysUnlockFromISR();
}

/**
 * @brief   Writes to a dedicated packet buffer.
 *
 * @param[in] udp       pointer to a @p stm32_usb_descriptor_t
 * @param[in] buf       buffer where to fetch the packet data
 * @param[in] n         maximum number of bytes to copy. This value must
 *                      not exceed the maximum packet size for this endpoint.
 *
 * @notapi
 */
static void usb_packet_write_from_buffer(stm32_usb_descriptor_t *udp,
                                         const uint8_t *buf,
                                         size_t n) {
  uint32_t w;
  size_t i;
  stm32_usb_pma_t *pmap;

  pmap = USB_ADDR2PTR(udp->TXADDR0);

  /* Pushing all complete words.*/
  i = 0;
  w = 0; /* Useless but silences a warning.*/
  while (i < n) {
    if ((i & 1) == 0) {
      w = (uint32_t)*buf;
    }
    else {
      w |= (uint32_t)*buf << 8;
      *pmap = (stm32_usb_pma_t)w;
      pmap++;
    }
    i++;
    buf++;
  }

  /* Remaining byte.*/
  if ((i & 1) != 0) {
    *pmap = (stm32_usb_pma_t)w;
  }
}

/**
 * @brief   Writes to a dedicated packet buffer.
 *
 * @param[in] udp       pointer to a @p stm32_usb_descriptor_t
 * @param[in] buf       buffer where to fetch the packet data
 * @param[in] n         maximum number of bytes to copy. This value must
 *                      not exceed the maximum packet size for this endpoint.
 *
 * @notapi
 */
static void usb_packet_write_from_queue(stm32_usb_descriptor_t *udp,
                                        output_queue_t *oqp, size_t n) {
  size_t nhw;
  syssts_t sts;
  stm32_usb_pma_t *pmap = USB_ADDR2PTR(udp->TXADDR0);

  nhw = n / 2;
  while (nhw > 0) {
    stm32_usb_pma_t w;

    w  = (stm32_usb_pma_t)*oqp->q_rdptr++;
    if (oqp->q_rdptr >= oqp->q_top)
      oqp->q_rdptr = oqp->q_buffer;
    w |= (stm32_usb_pma_t)*oqp->q_rdptr++ << 8;
    if (oqp->q_rdptr >= oqp->q_top)
      oqp->q_rdptr = oqp->q_buffer;
    *pmap++ = w;
    nhw--;
  }

  /* Last byte for odd numbers.*/
  if ((n & 1) != 0) {
    *pmap = (stm32_usb_pma_t)*oqp->q_rdptr++;
    if (oqp->q_rdptr >= oqp->q_top)
      oqp->q_rdptr = oqp->q_buffer;
  }

  /* Updating queue.*/
  sts = osalSysGetStatusAndLockX();

  oqp->q_counter += n;
  osalThreadDequeueAllI(&oqp->q_waiting, Q_OK);

  osalSysRestoreStatusX(sts);
}

/*===========================================================================*/
/* Driver interrupt handlers.                                                */
/*===========================================================================*/

#if STM32_USB_USE_USB1 || defined(__DOXYGEN__)
#if STM32_USB1_HP_NUMBER != STM32_USB1_LP_NUMBER
/**
 * @brief   USB high priority interrupt handler.
 *
 * @isr
 */
OSAL_IRQ_HANDLER(STM32_USB1_HP_HANDLER) {

  OSAL_IRQ_PROLOGUE();

  OSAL_IRQ_EPILOGUE();
}
#endif /* STM32_USB1_LP_NUMBER != STM32_USB1_HP_NUMBER */

/**
 * @brief   USB low priority interrupt handler.
 *
 * @isr
 */
OSAL_IRQ_HANDLER(STM32_USB1_LP_HANDLER) {
  uint32_t istr;
  USBDriver *usbp = &USBD1;

  OSAL_IRQ_PROLOGUE();

  istr = STM32_USB->ISTR;

  /* USB bus reset condition handling.*/
  if (istr & ISTR_RESET) {
    STM32_USB->ISTR = ~ISTR_RESET;

    _usb_reset(usbp);
  }

  /* USB bus SUSPEND condition handling.*/
  if (istr & ISTR_SUSP) {
    STM32_USB->CNTR |= CNTR_FSUSP;
#if STM32_USB_LOW_POWER_ON_SUSPEND
    STM32_USB->CNTR |= CNTR_LP_MODE;
#endif
    STM32_USB->ISTR = ~ISTR_SUSP;

    _usb_suspend(usbp);
  }

  /* USB bus WAKEUP condition handling.*/
  if (istr & ISTR_WKUP) {
    uint32_t fnr = STM32_USB->FNR;
    if (!(fnr & FNR_RXDP)) {
      STM32_USB->CNTR &= ~CNTR_FSUSP;

      _usb_wakeup(usbp);
    }
#if STM32_USB_LOW_POWER_ON_SUSPEND
    else {
      /* Just noise, going back in SUSPEND mode, reference manual 22.4.5,
         table 169.*/
      STM32_USB->CNTR |= CNTR_LP_MODE;
    }
#endif
    STM32_USB->ISTR = ~ISTR_WKUP;
  }

  /* SOF handling.*/
  if (istr & ISTR_SOF) {
    _usb_isr_invoke_sof_cb(usbp);
    STM32_USB->ISTR = ~ISTR_SOF;
  }

  /* Endpoint events handling.*/
  while (istr & ISTR_CTR) {
    size_t n;
    uint32_t ep;
    uint32_t epr = STM32_USB->EPR[ep = istr & ISTR_EP_ID_MASK];
    const USBEndpointConfig *epcp = usbp->epc[ep];

    if (epr & EPR_CTR_TX) {
      size_t transmitted;
      /* IN endpoint, transmission.*/
      EPR_CLEAR_CTR_TX(ep);

      /* Double buffering is always enabled for isochronous endpoints, and
         although we overlap the two buffers for simplicity, we still need
         to read from the right counter. The DTOG_TX bit indicates the buffer
         that is currently in use by the USB peripheral, that is, the buffer
         from which the next packet will be sent, so we need to read the
         transmitted bytes from the counter of the OTHER buffer, which is
         where we stored the last transmitted packet.*/
      transmitted = (size_t)USB_GET_DESCRIPTOR(ep)->TXCOUNT0;
      if (EPR_EP_TYPE_IS_ISO(epr) && !(epr & EPR_DTOG_TX))
        transmitted = (size_t)USB_GET_DESCRIPTOR(ep)->TXCOUNT1;

      epcp->in_state->txcnt  += transmitted;
      n = epcp->in_state->txsize - epcp->in_state->txcnt;
      if (n > 0) {
        /* Transfer not completed, there are more packets to send.*/
        if (n > epcp->in_maxsize)
          n = epcp->in_maxsize;

        /* Double buffering is always enabled for isochronous endpoints, and
           although we overlap the two buffers for simplicity, we still need
           to write to the right counter. The DTOG_TX bit indicates the buffer
           that is currently in use by the USB peripheral, that is, the buffer
           from which the next packet will be sent, so we need to write the
           counter of that buffer.*/
        USB_GET_DESCRIPTOR(ep)->TXCOUNT0 = (stm32_usb_pma_t)n;
        if (EPR_EP_TYPE_IS_ISO(epr) && (epr & EPR_DTOG_TX))
            USB_GET_DESCRIPTOR(ep)->TXCOUNT1 = (stm32_usb_pma_t)n;

        if (epcp->in_state->txqueued)
          usb_packet_write_from_queue(USB_GET_DESCRIPTOR(ep),
                                      epcp->in_state->mode.queue.txqueue,
                                      n);
        else {
          epcp->in_state->mode.linear.txbuf += transmitted;
          usb_packet_write_from_buffer(USB_GET_DESCRIPTOR(ep),
                                       epcp->in_state->mode.linear.txbuf,
                                       n);
        }
        osalSysLockFromISR();
        usb_lld_start_in(usbp, ep);
        osalSysUnlockFromISR();
      }
      else {
        /* Transfer completed, invokes the callback.*/
        _usb_isr_invoke_in_cb(usbp, ep);
      }
    }
    if (epr & EPR_CTR_RX) {
      EPR_CLEAR_CTR_RX(ep);
      /* OUT endpoint, receive.*/
      if (epr & EPR_SETUP) {
        /* Setup packets handling, setup packets are handled using a
           specific callback.*/
        _usb_isr_invoke_setup_cb(usbp, ep);
      }
      else {
        stm32_usb_descriptor_t *udp = USB_GET_DESCRIPTOR(ep);

        /* Double buffering is always enabled for isochronous endpoints, and
           although we overlap the two buffers for simplicity, we still need
           to read from the right counter. The DTOG_RX bit indicates the buffer
           that is currently in use by the USB peripheral, that is, the buffer
           in which the next received packet will be stored, so we need to
           read the counter of the OTHER buffer, which is where the last
           received packet was stored.*/
        n = (size_t)udp->RXCOUNT0 & RXCOUNT_COUNT_MASK;
        if (EPR_EP_TYPE_IS_ISO(epr) && !(epr & EPR_DTOG_RX))
          n = (size_t)udp->RXCOUNT1 & RXCOUNT_COUNT_MASK;

        /* Reads the packet into the defined buffer.*/
        if (epcp->out_state->rxqueued)
          usb_packet_read_to_queue(udp,
                                   epcp->out_state->mode.queue.rxqueue,
                                   n);
        else {
          usb_packet_read_to_buffer(udp,
                                    epcp->out_state->mode.linear.rxbuf,
                                    n);
          epcp->out_state->mode.linear.rxbuf += n;
        }

        /* Transaction data updated.*/
        epcp->out_state->rxcnt              += n;
        epcp->out_state->rxsize             -= n;
        epcp->out_state->rxpkts             -= 1;

        /* The transaction is completed if the specified number of packets
           has been received or the current packet is a short packet.*/
        if ((n < epcp->out_maxsize) || (epcp->out_state->rxpkts == 0)) {
          /* Transfer complete, invokes the callback.*/
          _usb_isr_invoke_out_cb(usbp, ep);
        }
        else {
          /* Transfer not complete, there are more packets to receive.*/
          EPR_SET_STAT_RX(ep, EPR_STAT_RX_VALID);
        }
      }
    }
    istr = STM32_USB->ISTR;
  }

  OSAL_IRQ_EPILOGUE();
}
#endif /* STM32_USB_USE_USB1 */

/*===========================================================================*/
/* Driver exported functions.                                                */
/*===========================================================================*/

/**
 * @brief   Low level USB driver initialization.
 *
 * @notapi
 */
void usb_lld_init(void) {

  /* Driver initialization.*/
  usbObjectInit(&USBD1);
}

/**
 * @brief   Configures and activates the USB peripheral.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 *
 * @notapi
 */
void usb_lld_start(USBDriver *usbp) {

  if (usbp->state == USB_STOP) {
    /* Clock activation.*/
#if STM32_USB_USE_USB1
    if (&USBD1 == usbp) {
      /* USB clock enabled.*/
      rccEnableUSB(FALSE);
      /* Powers up the transceiver while holding the USB in reset state.*/
      STM32_USB->CNTR = CNTR_FRES;
      /* Enabling the USB IRQ vectors, this also gives enough time to allow
         the transceiver power up (1uS).*/
#if STM32_USB1_HP_NUMBER != STM32_USB1_LP_NUMBER
      nvicEnableVector(STM32_USB1_HP_NUMBER, STM32_USB_USB1_HP_IRQ_PRIORITY);
#endif
      nvicEnableVector(STM32_USB1_LP_NUMBER, STM32_USB_USB1_LP_IRQ_PRIORITY);
      /* Releases the USB reset.*/
      STM32_USB->CNTR = 0;
    }
#endif
    /* Reset procedure enforced on driver start.*/
    _usb_reset(usbp);
  }
  /* Configuration.*/
}

/**
 * @brief   Deactivates the USB peripheral.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 *
 * @notapi
 */
void usb_lld_stop(USBDriver *usbp) {

  /* If in ready state then disables the USB clock.*/
  if (usbp->state == USB_STOP) {
#if STM32_USB_USE_USB1
    if (&USBD1 == usbp) {
#if STM32_USB1_HP_NUMBER != STM32_USB1_LP_NUMBER
      nvicDisableVector(STM32_USB1_HP_NUMBER);
#endif
      nvicDisableVector(STM32_USB1_LP_NUMBER);
      STM32_USB->CNTR = CNTR_PDWN | CNTR_FRES;
      rccDisableUSB(FALSE);
    }
#endif
  }
}

/**
 * @brief   USB low level reset routine.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 *
 * @notapi
 */
void usb_lld_reset(USBDriver *usbp) {
  uint32_t cntr;

  /* Post reset initialization.*/
  STM32_USB->BTABLE = BTABLE_ADDR;
  STM32_USB->ISTR   = 0;
  STM32_USB->DADDR  = DADDR_EF;
  cntr              = /*CNTR_ESOFM | */ CNTR_RESETM  | CNTR_SUSPM |
                      CNTR_WKUPM | /*CNTR_ERRM | CNTR_PMAOVRM |*/ CNTR_CTRM;
  /* The SOF interrupt is only enabled if a callback is defined for
     this service because it is an high rate source.*/
  if (usbp->config->sof_cb != NULL)
    cntr |= CNTR_SOFM;
  STM32_USB->CNTR = cntr;

  /* Resets the packet memory allocator.*/
  usb_pm_reset(usbp);

  /* EP0 initialization.*/
  usbp->epc[0] = &ep0config;
  usb_lld_init_endpoint(usbp, 0);
}

/**
 * @brief   Sets the USB address.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 *
 * @notapi
 */
void usb_lld_set_address(USBDriver *usbp) {

  STM32_USB->DADDR = (uint32_t)(usbp->address) | DADDR_EF;
}

/**
 * @brief   Enables an endpoint.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 * @param[in] ep        endpoint number
 *
 * @notapi
 */
void usb_lld_init_endpoint(USBDriver *usbp, usbep_t ep) {
  uint16_t nblocks, epr;
  stm32_usb_descriptor_t *dp;
  const USBEndpointConfig *epcp = usbp->epc[ep];

  /* Setting the endpoint type. Note that isochronous endpoints cannot be
     bidirectional because it uses double buffering and both transmit and
     receive descriptor fields are used for either direction.*/
  switch (epcp->ep_mode & USB_EP_MODE_TYPE) {
  case USB_EP_MODE_TYPE_ISOC:
    osalDbgAssert((epcp->in_cb == NULL) || (epcp->out_cb == NULL),
                  "isochronous EP cannot be IN and OUT");
    epr = EPR_EP_TYPE_ISO;
    break;
  case USB_EP_MODE_TYPE_BULK:
    epr = EPR_EP_TYPE_BULK;
    break;
  case USB_EP_MODE_TYPE_INTR:
    epr = EPR_EP_TYPE_INTERRUPT;
    break;
  default:
    epr = EPR_EP_TYPE_CONTROL;
  }

  /* Endpoint size and address initialization.*/
  if (epcp->out_maxsize > 62)
    nblocks = (((((epcp->out_maxsize - 1) | 0x1f) + 1) / 32) << 10) |
              0x8000;
  else
    nblocks = ((((epcp->out_maxsize - 1) | 1) + 1) / 2) << 10;

  dp = USB_GET_DESCRIPTOR(ep);
  dp->TXCOUNT0 = 0;
  dp->RXCOUNT0 = nblocks;
  dp->TXADDR0  = usb_pm_alloc(usbp, epcp->in_maxsize);
  dp->RXADDR0  = usb_pm_alloc(usbp, epcp->out_maxsize);

  /* Initial status for isochronous enpoints is valid because disabled and
     valid are the only legal values. Also since double buffering is used
     we need to initialize both count/address sets depending on the direction,
     but since we are not taking advantage of the double buffering, we set both
     addresses to point to the same PMA.*/
  if ((epcp->ep_mode & USB_EP_MODE_TYPE) == USB_EP_MODE_TYPE_ISOC) {
    if (epcp->in_cb != NULL) {
      epr |= EPR_STAT_TX_VALID;
      dp->TXCOUNT1 = dp->TXCOUNT0;
      dp->TXADDR1  = dp->TXADDR0;   /* Both buffers overlapped.*/
    }
    if (epcp->out_cb != NULL) {
      epr |= EPR_STAT_RX_VALID;
      dp->RXCOUNT1 = dp->RXCOUNT0;
      dp->RXADDR1  = dp->RXADDR0;   /* Both buffers overlapped.*/
    }
  }
  else {
    /* Initial status for other endpoint types is NAK.*/
    if (epcp->in_cb != NULL)
      epr |= EPR_STAT_TX_NAK;

    if (epcp->out_cb != NULL)
      epr |= EPR_STAT_RX_NAK;
  }

  /* EPxR register setup.*/
  EPR_SET(ep, epr | ep);
  EPR_TOGGLE(ep, epr);
}

/**
 * @brief   Disables all the active endpoints except the endpoint zero.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 *
 * @notapi
 */
void usb_lld_disable_endpoints(USBDriver *usbp) {
  unsigned i;

  /* Resets the packet memory allocator.*/
  usb_pm_reset(usbp);

  /* Disabling all endpoints.*/
  for (i = 1; i <= USB_ENDOPOINTS_NUMBER; i++) {
    EPR_TOGGLE(i, 0);
    EPR_SET(i, 0);
  }
}

/**
 * @brief   Returns the status of an OUT endpoint.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 * @param[in] ep        endpoint number
 * @return              The endpoint status.
 * @retval EP_STATUS_DISABLED The endpoint is not active.
 * @retval EP_STATUS_STALLED  The endpoint is stalled.
 * @retval EP_STATUS_ACTIVE   The endpoint is active.
 *
 * @notapi
 */
usbepstatus_t usb_lld_get_status_out(USBDriver *usbp, usbep_t ep) {

  (void)usbp;
  switch (STM32_USB->EPR[ep] & EPR_STAT_RX_MASK) {
  case EPR_STAT_RX_DIS:
    return EP_STATUS_DISABLED;
  case EPR_STAT_RX_STALL:
    return EP_STATUS_STALLED;
  default:
    return EP_STATUS_ACTIVE;
  }
}

/**
 * @brief   Returns the status of an IN endpoint.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 * @param[in] ep        endpoint number
 * @return              The endpoint status.
 * @retval EP_STATUS_DISABLED The endpoint is not active.
 * @retval EP_STATUS_STALLED  The endpoint is stalled.
 * @retval EP_STATUS_ACTIVE   The endpoint is active.
 *
 * @notapi
 */
usbepstatus_t usb_lld_get_status_in(USBDriver *usbp, usbep_t ep) {

  (void)usbp;
  switch (STM32_USB->EPR[ep] & EPR_STAT_TX_MASK) {
  case EPR_STAT_TX_DIS:
    return EP_STATUS_DISABLED;
  case EPR_STAT_TX_STALL:
    return EP_STATUS_STALLED;
  default:
    return EP_STATUS_ACTIVE;
  }
}

/**
 * @brief   Reads a setup packet from the dedicated packet buffer.
 * @details This function must be invoked in the context of the @p setup_cb
 *          callback in order to read the received setup packet.
 * @pre     In order to use this function the endpoint must have been
 *          initialized as a control endpoint.
 * @post    The endpoint is ready to accept another packet.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 * @param[in] ep        endpoint number
 * @param[out] buf      buffer where to copy the packet data
 *
 * @notapi
 */
void usb_lld_read_setup(USBDriver *usbp, usbep_t ep, uint8_t *buf) {
  stm32_usb_pma_t *pmap;
  stm32_usb_descriptor_t *udp;
  uint32_t n;

  (void)usbp;
  udp = USB_GET_DESCRIPTOR(ep);
  pmap = USB_ADDR2PTR(udp->RXADDR0);
  for (n = 0; n < 4; n++) {
    *(uint16_t *)buf = (uint16_t)*pmap++;
    buf += 2;
  }
}

/**
 * @brief   Prepares for a receive operation.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 * @param[in] ep        endpoint number
 *
 * @notapi
 */
void usb_lld_prepare_receive(USBDriver *usbp, usbep_t ep) {
  USBOutEndpointState *osp = usbp->epc[ep]->out_state;

  /* Transfer initialization.*/
  if (osp->rxsize == 0)         /* Special case for zero sized packets.*/
    osp->rxpkts = 1;
  else
    osp->rxpkts = (uint16_t)((osp->rxsize + usbp->epc[ep]->out_maxsize - 1) /
                             usbp->epc[ep]->out_maxsize);
}

/**
 * @brief   Prepares for a transmit operation.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 * @param[in] ep        endpoint number
 *
 * @notapi
 */
void usb_lld_prepare_transmit(USBDriver *usbp, usbep_t ep) {
  size_t n;
  USBInEndpointState *isp = usbp->epc[ep]->in_state;
  uint32_t epr = STM32_USB->EPR[ep];

  /* Transfer initialization.*/
  n = isp->txsize;
  if (n > (size_t)usbp->epc[ep]->in_maxsize)
    n = (size_t)usbp->epc[ep]->in_maxsize;

  /* Double buffering is always enabled for isochronous endpoints, and
     although we overlap the two buffers for simplicity, we still need
     to write to the right counter. The DTOG_TX bit indicates the buffer
     that is currently in use by the USB peripheral, that is, the buffer
     from which the next packet will be sent, so we need to write the
     counter of that buffer.*/
  USB_GET_DESCRIPTOR(ep)->TXCOUNT0 = (stm32_usb_pma_t)n;
  if (EPR_EP_TYPE_IS_ISO(epr) && (epr & EPR_DTOG_TX))
    USB_GET_DESCRIPTOR(ep)->TXCOUNT1 = (stm32_usb_pma_t)n;

  if (isp->txqueued)
    usb_packet_write_from_queue(USB_GET_DESCRIPTOR(ep),
                                isp->mode.queue.txqueue, n);
  else
    usb_packet_write_from_buffer(USB_GET_DESCRIPTOR(ep),
                                 isp->mode.linear.txbuf, n);
}

/**
 * @brief   Starts a receive operation on an OUT endpoint.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 * @param[in] ep        endpoint number
 *
 * @notapi
 */
void usb_lld_start_out(USBDriver *usbp, usbep_t ep) {

  (void)usbp;

  EPR_SET_STAT_RX(ep, EPR_STAT_RX_VALID);
}

/**
 * @brief   Starts a transmit operation on an IN endpoint.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 * @param[in] ep        endpoint number
 *
 * @notapi
 */
void usb_lld_start_in(USBDriver *usbp, usbep_t ep) {

  (void)usbp;

  EPR_SET_STAT_TX(ep, EPR_STAT_TX_VALID);
}

/**
 * @brief   Brings an OUT endpoint in the stalled state.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 * @param[in] ep        endpoint number
 *
 * @notapi
 */
void usb_lld_stall_out(USBDriver *usbp, usbep_t ep) {

  (void)usbp;

  EPR_SET_STAT_RX(ep, EPR_STAT_RX_STALL);
}

/**
 * @brief   Brings an IN endpoint in the stalled state.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 * @param[in] ep        endpoint number
 *
 * @notapi
 */
void usb_lld_stall_in(USBDriver *usbp, usbep_t ep) {

  (void)usbp;

  EPR_SET_STAT_TX(ep, EPR_STAT_TX_STALL);
}

/**
 * @brief   Brings an OUT endpoint in the active state.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 * @param[in] ep        endpoint number
 *
 * @notapi
 */
void usb_lld_clear_out(USBDriver *usbp, usbep_t ep) {

  (void)usbp;

  /* Makes sure to not put to NAK an endpoint that is already
     transferring.*/
  if ((STM32_USB->EPR[ep] & EPR_STAT_RX_MASK) != EPR_STAT_RX_VALID)
    EPR_SET_STAT_TX(ep, EPR_STAT_RX_NAK);
}

/**
 * @brief   Brings an IN endpoint in the active state.
 *
 * @param[in] usbp      pointer to the @p USBDriver object
 * @param[in] ep        endpoint number
 *
 * @notapi
 */
void usb_lld_clear_in(USBDriver *usbp, usbep_t ep) {

  (void)usbp;

  /* Makes sure to not put to NAK an endpoint that is already
     transferring.*/
  if ((STM32_USB->EPR[ep] & EPR_STAT_TX_MASK) != EPR_STAT_TX_VALID)
    EPR_SET_STAT_TX(ep, EPR_STAT_TX_NAK);
}

#endif /* HAL_USE_USB */

/** @} */
