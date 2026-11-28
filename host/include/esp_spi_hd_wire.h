// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-only
#ifndef ESP_SPI_HD_WIRE_H
#define ESP_SPI_HD_WIRE_H

/* SPI-HD shared-buffer register layout and on-wire command encoding. */
#define SPIHD_CMD_WRBUF                 0x01
#define SPIHD_CMD_RDBUF                 0x02
#define SPIHD_CMD_WRDMA                 0x03
#define SPIHD_CMD_RDDMA                 0x04
#define SPIHD_CMD_WR_END                0x07
#define SPIHD_CMD_INT0                  0x08
#define SPIHD_CMD_INT1                  0x09
#define SPIHD_QIO_MASK                  0xA0
#define SPIHD_QIO_DATA_ONLY_MASK        0x20

#define SPIHD_REG_SLAVE_READY           0x00
#define SPIHD_REG_MAX_TX_BUF_LEN        0x04
#define SPIHD_REG_MAX_RX_BUF_LEN        0x08
#define SPIHD_REG_TX_BUF_LEN            0x0C
/* Cumulative little-endian RX credit counter; the outstanding window
 * is strictly smaller than 256. */
#define SPIHD_REG_RX_BUF_LEN            0x10
#define SPIHD_REG_SLAVE_CTRL            0x14
#define SPIHD_REG_TX_LEN_MIRROR         0x18

#define SPIHD_STATE_SLAVE_READY         0xEE
#define SPIHD_CAP_TX_LEN_MIRROR          (1U << 8)
#define SPIHD_CAP_DMA_APPEND             (1U << 9)
#define SPIHD_CAP_PROTOCOL_V3            (1U << 12)
#define SPIHD_CTRL_DATAPATH_ON          (1U << 0)

#define SPIHD_TX_BUF_LEN_MASK           0x00FFFFFFU
#define SPIHD_INT_MASK                  (3U << 24)
#define SPIHD_REG_SESSION_LO             0x20
#define SPIHD_REG_SESSION_HI             0x24
#define SPIHD_REG_RX_CREDIT_WINDOW       0x28
#define SPIHD_MAX_RX_CREDIT_WINDOW       32U
#define SPIHD_REG_CONTROL_MAX_HZ         0x2C
#define SPIHD_REG_PAYLOAD_MAX_HZ         0x30
#define SPIHD_REG_HOST_RX_MAX            0x34
#define SPIHD_REGISTER_BYTES             0x38U

/* Padding is zero and excluded from the Hosted header length/checksum. */
#define SPIHD_FRAME_ALIGNMENT           4U
#define SPIHD_WIRE_SPAN(len)             (((len) + SPIHD_FRAME_ALIGNMENT - 1U) & \
                                          ~(SPIHD_FRAME_ALIGNMENT - 1U))

#endif
