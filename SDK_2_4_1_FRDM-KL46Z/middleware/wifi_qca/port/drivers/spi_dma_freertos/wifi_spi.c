/*
 * The Clear BSD License
 * Copyright (c) 2016, NXP Semiconductor, Inc.
 * All rights reserved.
 *
 * 
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted (subject to the limitations in the disclaimer below) provided
 *  that the following conditions are met:
 *
 * o Redistributions of source code must retain the above copyright notice, this list
 *   of conditions and the following disclaimer.
 *
 * o Redistributions in binary form must reproduce the above copyright notice, this
 *   list of conditions and the following disclaimer in the documentation and/or
 *   other materials provided with the distribution.
 *
 * o Neither the name of NXP Semiconductor, Inc. nor the names of its
 *   contributors may be used tom  endorse or promote products derived from this
 *   software without specific prior written permission.
 *
 * NO EXPRESS OR IMPLIED LICENSES TO ANY PARTY'S PATENT RIGHTS ARE GRANTED BY THIS LICENSE.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "wifi_common.h"
#include "athdefs.h"

#include "fsl_spi.h"
#include "fsl_dmamux.h"
#include "fsl_dma.h"
#include "fsl_spi_dma.h"
#include "wifi_spi.h"

static spi_dma_handle_t g_spi_dma_m_handle;
static dma_handle_t spiDmaMasterTxHandle;
static dma_handle_t spiDmaMasterRxHandle;
static spi_master_handle_t g_m_handle;

static SemaphoreHandle_t mutex;
static SemaphoreHandle_t event;
static int32_t g_dma_chunk = 0xFFFFF;
static enum IRQn g_dma_irqs[][FSL_FEATURE_DMA_MODULE_CHANNEL] = DMA_CHN_IRQS;
static enum IRQn g_spi_irqs[] = SPI_IRQS;
static spi_master_config_t g_spi_config;

static SPI_Type *g_spi_base = NULL;
static uint32_t g_irq_threshold = 0;

extern uint32_t SPI_GetInstance(SPI_Type *base);

/* taken from fsl_dma.c */
static DMA_Type *const s_dmaBases[] = DMA_BASE_PTRS;
static uint32_t DMA_GetInstance(DMA_Type *base)
{
    uint32_t instance;

    /* Find the instance index from base address mappings. */
    for (instance = 0; instance < ARRAY_SIZE(s_dmaBases); instance++)
    {
        if (s_dmaBases[instance] == base)
        {
            break;
        }
    }

    assert(instance < ARRAY_SIZE(s_dmaBases));

    return instance;
}

/*
 * DMA handler, release transfer semaphore
 */
static void SPI_DMA_MasterUserCallback(SPI_Type *base, spi_dma_handle_t *handle, status_t status, void *userData)
{
    /* disable DMA requests before invoke callback */
    BaseType_t reschedule = pdFALSE;
    xSemaphoreGiveFromISR(event, &reschedule);
    portYIELD_FROM_ISR(reschedule);
}

/* 
 * IRQ handler, release transfer semaphore
 */
static void SPI_MasterUserCallback(SPI_Type *base, spi_master_handle_t *handle, status_t status, void *userData)
{
    /* disable IRQ requests before invoke callback */
    BaseType_t reschedule = pdFALSE;
    xSemaphoreGiveFromISR(event, &reschedule);
    portYIELD_FROM_ISR(reschedule);
}

/*
 * Initialize SPI IRQ mode
 */
static A_STATUS WIFIDRVS_SPI_InitIRQMode(WIFIDRVS_SPI_config_t *config)
{
    uint32_t spi_id = SPI_GetInstance(config->spi.base);
    NVIC_SetPriority(g_spi_irqs[spi_id], config->irq_mode.spi_irq_prio);

    /* SPI IRQ non-blocking handle */
    SPI_MasterTransferCreateHandle(config->spi.base, &g_m_handle, SPI_MasterUserCallback, NULL);

    return A_OK;
}

/*
 * Initialize SPI DMA mode
 */
static A_STATUS WIFIDRVS_SPI_InitDMAMode(WIFIDRVS_SPI_config_t *config)
{
    assert(!((config->dma_mode.dma_rx_chnl < 0) || (config->dma_mode.dma_tx_chnl < 0)));
    if ((config->dma_mode.dma_rx_chnl < 0) || (config->dma_mode.dma_tx_chnl < 0))
    {
        return A_ERROR;
    }

    DMAMUX_Init(config->dma_mode.dmamux_base);
    DMAMUX_SetSource(config->dma_mode.dmamux_base, config->dma_mode.dma_rx_chnl, config->dma_mode.dmamux_rx_req);
    DMAMUX_EnableChannel(config->dma_mode.dmamux_base, config->dma_mode.dma_rx_chnl);
    DMAMUX_SetSource(config->dma_mode.dmamux_base, config->dma_mode.dma_tx_chnl, config->dma_mode.dmamux_tx_req);
    DMAMUX_EnableChannel(config->dma_mode.dmamux_base, config->dma_mode.dma_tx_chnl);

    /* Init DMA */
    DMA_Init(config->dma_mode.dma_base);

    /* clear DMA/SPI handles */
    memset(&spiDmaMasterRxHandle, 0, sizeof(spiDmaMasterRxHandle));
    memset(&spiDmaMasterTxHandle, 0, sizeof(spiDmaMasterTxHandle));
    DMA_CreateHandle(&spiDmaMasterRxHandle, config->dma_mode.dma_base, config->dma_mode.dma_rx_chnl);
    DMA_CreateHandle(&spiDmaMasterTxHandle, config->dma_mode.dma_base, config->dma_mode.dma_tx_chnl);
    SPI_MasterTransferCreateHandleDMA(config->spi.base, &g_spi_dma_m_handle, SPI_DMA_MasterUserCallback, NULL,
                                      &spiDmaMasterTxHandle, &spiDmaMasterRxHandle);

    uint32_t edma_id = DMA_GetInstance(config->dma_mode.dma_base);
    NVIC_SetPriority(g_dma_irqs[edma_id][config->dma_mode.dma_rx_chnl], config->dma_mode.dma_irq_prio);
    NVIC_SetPriority(g_dma_irqs[edma_id][config->dma_mode.dma_tx_chnl], config->dma_mode.dma_irq_prio);

    return A_OK;
}

/*
 * Initialize SPI peripheral
 */
static A_STATUS WIFIDRVS_SPI_InitPeriph(
    SPI_Type *base,
    uint32_t src_clk_hz,
    spi_master_config_t *user_config
)
{
    assert(NULL != base);
    assert(NULL != user_config);

    mutex = xSemaphoreCreateMutex();
    assert(NULL != mutex);
    event = xSemaphoreCreateBinary();
    assert(NULL != event);

    /* SPI init */
    SPI_MasterInit(base, &g_spi_config, src_clk_hz);

    return A_OK;
}

/*
 * Transfer data in DMA mode
 */
static A_STATUS WIFIDRVS_SPI_DMA_Transfer(spi_transfer_t *transfer)
{
    assert(NULL != transfer);
    status_t result = SPI_MasterTransferDMA(g_spi_base, &g_spi_dma_m_handle, transfer);
    if (kStatus_Success != result)
    {
        assert(0);
        return A_ERROR;
    }
    /* semaphore is released in callback fn */
    if (pdTRUE != xSemaphoreTake(event, portMAX_DELAY))
    {
        assert(0);
        return A_ERROR;
    }
    return A_OK;
}

/*
 * Transfer data in IRQ mode
 */
static A_STATUS WIFIDRVS_SPI_IRQ_Transfer(spi_transfer_t *transfer)
{
    assert(NULL != transfer);

    status_t result = SPI_MasterTransferNonBlocking(g_spi_base, &g_m_handle, transfer);
    if (kStatus_Success != result)
    {
        assert(0);
        return A_ERROR;
    }
    if (pdTRUE != xSemaphoreTake(event, portMAX_DELAY))
    {
        assert(0);
        result = A_ERROR;
    }
    return A_OK;
}

/*
 * Transfer data
 */
static A_STATUS WIFIDRVS_SPI_Transfer(spi_transfer_t *transfer)
{
    A_STATUS result = A_OK;

    /* NOTE: following code expects that SDK drivers do not
     * modify members of 'transfer' argument */
    for (int32_t to_transfer = transfer->dataSize; to_transfer;)
    {
        if (to_transfer < g_irq_threshold)
        {
            /* DMA is unefficient for small amount of data, so use IRQ mode.
             * IRQ mode can transfer unlimited number of data */
            transfer->dataSize = to_transfer;
            result = WIFIDRVS_SPI_IRQ_Transfer(transfer);
            if (A_OK != result)
                break;
            to_transfer = 0;
        }
        else
        {
            /* SPI over DMA is limited to 0xFFFFFF ~ 10MB so this loop is not necessary */
            transfer->dataSize = to_transfer < g_dma_chunk ? to_transfer : g_dma_chunk;
            result = WIFIDRVS_SPI_DMA_Transfer(transfer);
            if (A_OK != result)
                break;
            to_transfer -= transfer->dataSize;
            /* recalculate rx/rx offsets */
            if (NULL != transfer->txData)
            {
                transfer->txData += transfer->dataSize;
            }
            if (NULL != transfer->rxData)
            {
                transfer->rxData += transfer->dataSize;
            }
        }
    }

    return result;
}

/*!
 * @brief Initialize SPI driver
 */
A_STATUS WIFIDRVS_SPI_Init(WIFIDRVS_SPI_config_t *config)
{
    /* No SPI base address, invalid config*/
    assert(!((NULL == config) || (NULL == config->spi.base)));
    if ((NULL == config) || (NULL == config->spi.base)) return A_ERROR;

    /* IRQ mode only - set threshold to max value */
    if ((config->irq_mode.enabled) && (!config->dma_mode.enabled))
    {
        g_irq_threshold = (uint32_t)-1;
    }
    /* DMA mode only - set threshold to 0 */
    else if ((!config->irq_mode.enabled) && (config->dma_mode.enabled))
    {
        g_irq_threshold = 0;
    }
    /* DMA and IRQ mode - set user defined value  */
    else if ((config->irq_mode.enabled) && (config->dma_mode.enabled))
    {
        g_irq_threshold = config->spi.irq_threshold;
    }
    /* Neither of modes is enabled, return error  */
    else
    {
        return A_ERROR;
    }

    /* Prepare driver internal context */
    g_spi_base = config->spi.base;
    g_spi_config = config->spi.config;

    /* Initialize SPI peripheral */
    WIFIDRVS_SPI_InitPeriph(config->spi.base, config->spi.clk_hz, &config->spi.config);

    /* Enable IRQ mode */
    if (config->irq_mode.enabled)
    {
        WIFIDRVS_SPI_InitIRQMode(config);
    }

    /* Enable DMA mode */
    if (config->dma_mode.enabled)
    {
        WIFIDRVS_SPI_InitDMAMode(config);
    }

    return A_OK;
}

/*!
 * @brief Deinitialize SPI driver
 */
A_STATUS WIFIDRVS_SPI_Deinit(WIFIDRVS_SPI_config_t *config)
{
    assert(!(NULL == config));
    if (NULL == config) return A_ERROR;

    if (NULL == config->spi.base) return A_ERROR;
    SPI_Deinit(config->spi.base);

    return A_OK;
}

/*!
 * @brief Return default configuration
 */
A_STATUS WIFIDRVS_SPI_GetDefaultConfig(WIFIDRVS_SPI_config_t *config)
{
    assert(!(NULL == config));
    if (NULL == config) return A_ERROR;

    memset(config, 0, sizeof(*config));
    config->dma_mode.dma_rx_chnl = -1;
    config->dma_mode.dma_tx_chnl = -1;

    return A_OK;
}

/*!
 * @brief Return default SPI peripheral settings
 */
A_STATUS WIFIDRVS_SPI_GetSPIConfig(spi_master_config_t *user_config, uint32_t baudrate)
{
    assert(!(NULL == user_config));
    if (NULL == user_config) return A_ERROR;

    memset(user_config, 0, sizeof(*user_config));
    SPI_MasterGetDefaultConfig(user_config);
    user_config->enableMaster = true;
    user_config->enableStopInWaitMode = false;
    user_config->polarity = kSPI_ClockPolarityActiveLow;
    user_config->phase = kSPI_ClockPhaseSecondEdge;
    user_config->direction = kSPI_MsbFirst;
    user_config->dataMode = kSPI_8BitMode;
    user_config->txWatermark = kSPI_TxFifoOneHalfEmpty;
    user_config->rxWatermark = kSPI_RxFifoOneHalfFull;
    user_config->pinMode = kSPI_PinModeNormal;
    user_config->outputMode = kSPI_SlaveSelectAutomaticOutput;
    user_config->baudRate_Bps = baudrate;

    return A_OK;
}

/*!
 * @brief WiFi SPI transfer SPI
 */
A_STATUS WIFIDRVS_SPI_InOutToken(uint32_t OutToken, uint8_t DataSize, uint32_t *pInToken)
{
    A_STATUS result;
    spi_transfer_t transfer = {0};

    transfer.txData = (uint8_t *)&OutToken;
    transfer.rxData = (uint8_t *)pInToken;
    transfer.dataSize = DataSize;

    /* Protect transmit by mutex */
    if (pdTRUE != xSemaphoreTake(mutex, portMAX_DELAY))
    {
        return A_ERROR;
    }
    result = WIFIDRVS_SPI_Transfer(&transfer);
    xSemaphoreGive(mutex);
    return result;
}

/*!
 * @brief WiFi SPI transfer SPI
 */
A_STATUS WIFIDRVS_SPI_InOutBuffer(uint8_t *pBuffer, uint16_t length, uint8_t doRead, boolean sync)
{
    A_STATUS result;
    spi_transfer_t transfer = {0};

    if (doRead)
    {
        transfer.txData = NULL;
        transfer.rxData = pBuffer;
    }
    else
    {
        transfer.txData = pBuffer;
        transfer.rxData = NULL;
    }
    transfer.dataSize = length;

    /* Protect transmit by mutex */
    if (pdTRUE != xSemaphoreTake(mutex, portMAX_DELAY))
    {
        return A_ERROR;
    }
    result = WIFIDRVS_SPI_Transfer(&transfer);
    xSemaphoreGive(mutex);
    return result;
}

