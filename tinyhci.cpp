/*
All of tinyhci is licensed under the MIT license.

Copyright (c) 2014 by Wade Brainerd <wadeb@wadeb.com>

Permission is hereby granted, free of charge, to any person obtaining a copy of this software
and associated documentation files (the "Software"), to deal in the Software without restriction,
including without limitation the rights to use, copy, modify, merge, publish, distribute,
sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or
substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/
#include <Arduino.h>
#include <SPI.h>
#include "tinyhci.h"

#define USE_WATCHDOG 0

#if USE_WATCHDOG
#include <avr/wdt.h>
#else
#define wdt_reset()
#endif

//
// Define the fw version of the CC3000 module you are running against.
//
#define CC3000_FW_VERSION         132

//
// Redefine these based on your particular hardware.
//
#define CC3K_CS_PIN               6
#define CC3K_IRQ_PIN              7
#define CC3K_EN_PIN               5
#define CC3K_IRQ_NUM              4

//
// Global variables
//
volatile uint8_t wifi_connected = 0;
volatile uint8_t wifi_dhcp = 0;
volatile uint8_t ip_addr[4];

//
// Static variables
//
static volatile uint8_t hci_data_available;
static volatile uint8_t hci_pending_event_available;
static volatile uint16_t hci_pending_event = 0xffff;

static uint16_t hci_buffer_size;
static uint8_t hci_buffer_count;
static volatile uint8_t hci_available_buffer_count;

static uint16_t hci_payload_size;
static uint8_t hci_pad;

static volatile uint8_t hci_state;

//
// HCI interface constants
//
#define HCI_STATE_IDLE                          0
#define HCI_STATE_WAIT_ASSERT                   1

#define HCI_READ                                0x3
#define HCI_WRITE                               0x1

#define HCI_TYPE_CMND                           0x1
#define HCI_TYPE_DATA                           0x2
#define HCI_TYPE_PATCH                          0x3
#define HCI_TYPE_EVNT                           0x4

//
// HCI Command IDs
//
#define HCI_CMND_WLAN_CONNECT                   0x0001
#define HCI_CMND_WLAN_DISCONNECT                0x0002
#define HCI_CMND_WLAN_IOCTL_SET_CONNECTION_POLICY 0x0004
#define HCI_CMND_EVENT_MASK                     0x0008

#define HCI_CMND_SEND                           0x0081
#define HCI_CMND_SENDTO                         0x0083
#define HCI_DATA_BSD_RECVFROM                   0x0084
#define HCI_DATA_BSD_RECV                       0x0085

#define HCI_CMND_SOCKET                         0x1001
#define HCI_CMND_BIND                           0x1002
#define HCI_CMND_RECV                           0x1004
#define HCI_CMND_ACCEPT                         0x1005
#define HCI_CMND_LISTEN                         0x1006
#define HCI_CMND_CONNECT                        0x1007
#define HCI_CMND_SELECT                         0x1008
#define HCI_CMND_SETSOCKOPT                     0x1009
#define HCI_CMND_CLOSE_SOCKET                   0x100B
#define HCI_CMND_GETHOSTNAME                    0x1010
#define HCI_CMND_MDNS_ADVERTISE                 0x1011

#define HCI_NETAPP_SET_TIMERS                   0x2009

#define HCI_CMND_SIMPLE_LINK_START              0x4000
#define HCI_CMND_READ_BUFFER_SIZE               0x400B

//
// HCI Data commands
//
#define HCI_DATA_RECVFROM                       0x84
#define HCI_DATA_RECV                           0x85
#define HCI_DATA_NVMEM                          0x91

//
// HCI Command/Event argument constants
//
#define SL_PATCHES_REQUEST_DEFAULT              0

#define HCI_ATTR __attribute__((noinline))

#if USE_WATCHDOG
ISR(WDT_vect) // Watchdog timer interrupt.
{
    SERIAL_PRINTLN("WDT !");
}
#endif

void wifi_callback(uint16_t event, uint32_t arg);
//
// hci_transfer
//
// Transfers one byte across HCI, the basis of all CC3000 communications.
// Enable DEBUG_LV4 in tinyhci.h to watch the traffic via the serial port.
//
HCI_ATTR
uint8_t hci_transfer(uint8_t out)
{
  uint8_t in = SPI.transfer(out);
  DEBUG_LV4(
    Serial.print("SPI: ");
    Serial.print(out, HEX);
    Serial.print(" -> ");
    Serial.print(in, HEX);
    Serial.println();
    Serial.flush());
  return in;
}

//
// Low level SPI reading functions for various data types.
//
// Note that these respect the payload size from the most recently read HCI header,
//  and will not overrun it; preferring to return 0s instead.
//
HCI_ATTR
uint8_t hci_read_u8(void)
{
  if (hci_payload_size > 0)
  {
    hci_payload_size--;
    return hci_transfer(0);
  }
  else
  {
    return 0;
  }
}

HCI_ATTR
uint16_t hci_read_u16_le(void)
{
  uint8_t b0 = hci_read_u8();
  uint8_t b1 = hci_read_u8();
  return (uint16_t)b0 | ((uint16_t)b1 << 8);
}

HCI_ATTR
uint32_t hci_read_u32_le(void)
{
  uint8_t b0 = hci_read_u8();
  uint8_t b1 = hci_read_u8();
  uint8_t b2 = hci_read_u8();
  uint8_t b3 = hci_read_u8();

  return (uint32_t)b0 | ((uint32_t)b1 << 8) |
          ((uint32_t)b2 << 16) | ((uint32_t)b3 << 24);
}

HCI_ATTR
void hci_read_array(uint8_t *data, uint16_t length)
{
  while (length)
  {
    *data = hci_read_u8();
    data++;
    length--;
  }
}

//
// Low level SPI writing functions for various data types.
//
// Note that these respect the payload size from the most recently written HCI header,
//  and will not overrun it; sending nothing.
//
HCI_ATTR
uint8_t hci_write_u8(uint8_t v)
{
  if (hci_payload_size > 0)
  {
    hci_payload_size--;
    hci_transfer(v);
  }
}

HCI_ATTR
void hci_write_u16_le(uint16_t v)
{
  hci_write_u8((v >>  0) & 0xff);
  hci_write_u8((v >>  8) & 0xff);
}

HCI_ATTR
void hci_write_u32_le(uint32_t v)
{
  hci_write_u8((v >>  0) & 0xff);
  hci_write_u8((v >>  8) & 0xff);
  hci_write_u8((v >> 16) & 0xff);
  hci_write_u8((v >> 24) & 0xff);
}

HCI_ATTR
void hci_write_array(const void *data, uint16_t length)
{
  const uint8_t *pos = (uint8_t*)data;
  while (length)
  {
    hci_write_u8(*pos);
    pos++;
    length--;
  }
}

//
// hci_begin_receive
//
// Reads the headers for an HCI event or data message, called by the interrupt handler.
//
HCI_ATTR
void hci_begin_receive(void)
{
  DEBUG_LV3(SERIAL_PRINTFUNCTION());

  // Master Read Transaction

  // 1. The IRQ line is asserted by the CC3000 device.

  // 2. The master asserts the nCS line.
  digitalWrite(CC3K_CS_PIN, LOW);

  // 3. The master transmits the following 3 bytes: read opcode followed by two busy bytes
  hci_transfer(HCI_READ);
  hci_transfer(0);
  hci_transfer(0);

  // 4. The CC3000 sends back the following data: the first two bytes indicate the payload length
  //    and the data payload bytes follow, immediately after.
  uint8_t p0 = hci_transfer(0);
  uint8_t p1 = hci_transfer(0);

  hci_payload_size = (p0 << 8) | p1;
  DEBUG_LV3(SERIAL_PRINTVAR(hci_payload_size));
}

//
// hci_end_receive
//
// Finishes reading the current event or data message, discarding any unread portion.
// Called both inside and outside the interrupt handler.
//
HCI_ATTR
void hci_end_receive(void)
{
  DEBUG_LV3(
    SERIAL_PRINTFUNCTION();
    SERIAL_PRINTVAR(hci_payload_size);
    )

  // Read and discard any unread portion of the message.
  while (hci_payload_size)
    hci_read_u8();

  // 5. At the end of read transaction, the master drives nCS inactive.
  digitalWrite(CC3K_CS_PIN, HIGH);

  // 6. The CC3000 device deasserts an IRQ line.
  wdt_reset();
  while (digitalRead(CC3K_IRQ_PIN) == LOW)
    ; // intentionally no wdt_reset()
}

//
// hci_dispatch_event
//
// This is the main incoming event handler, called by the interrupt handler.
// It has three different modes of operation, depending on the event type.
//
// If we are waiting for the CC3000 to respond to a command with a specific event (hci_pending_event),
// we mark it as available and return.  The non-interrupt code will then take care of receiving the
// event contents.
//
// If it's an unsolicited event that we care about, we receive the event contents and handle them.
//
// If it's neither type, we discard all of the event contents.
//
HCI_ATTR
void hci_dispatch_event(void)
{
  uint16_t rx_event_type = hci_read_u16_le();
  DEBUG_LV3(SERIAL_PRINTVAR_HEX(rx_event_type));

  uint8_t rx_args_size = hci_read_u8();
  DEBUG_LV3(SERIAL_PRINTVAR(rx_args_size));

  uint32_t arg = 0;

  if (rx_event_type == hci_pending_event)
  {
    hci_pending_event_available = 1;
  }
  else
  {
    switch (rx_event_type)
    {
    case HCI_EVNT_WLAN_UNSOL_CONNECT:
      wifi_connected = 1;
      DEBUG_LV3(SERIAL_PRINTVAR(wifi_connected));
      break;

    case HCI_EVNT_WLAN_UNSOL_DISCONNECT:
      wifi_connected = 0;
      wifi_dhcp = 0;
      DEBUG_LV3(SERIAL_PRINTVAR(wifi_connected));
      DEBUG_LV3(SERIAL_PRINTVAR(wifi_dhcp));
      break;

    case HCI_EVNT_WLAN_UNSOL_DHCP:
      wifi_dhcp = 1;
      DEBUG_LV3(SERIAL_PRINTVAR(wifi_dhcp));
      hci_read_u8(); // status
      ip_addr[3] = hci_read_u8();
      ip_addr[2] = hci_read_u8();
      ip_addr[1] = hci_read_u8();
      ip_addr[0] = hci_read_u8();
      break;

    case HCI_EVNT_WLAN_UNSOL_TCP_CLOSE_WAIT:
      hci_read_u8(); // Status
      arg = hci_read_u32_le(); // Read socket number
      DEBUG_LV3(SERIAL_PRINTVAR(client_socket));
      break;

    case HCI_EVNT_DATA_UNSOL_FREE_BUFF:
      {
        hci_read_u8(); // status
        uint16_t fce_count = hci_read_u16_le();
        for (uint16_t i = 0; i < fce_count; i++)
        {
          hci_read_u16_le(); // ??
          hci_available_buffer_count += hci_read_u16_le();
        }
        DEBUG_LV3(SERIAL_PRINTVAR(hci_available_buffer_count));
      }
      break;

    default:
      break;
    }

    // Callback to user program.
    wifi_callback(rx_event_type, arg);

    hci_end_receive();
  }
}

//
// hci_dispatch_data
//
// This is the incoming data handler, called by the interrupt handler.
// It skips over the data arguments and flags data as being available, before returning
// from the interrupt.  We are presumed to be inside a function that is expecting data.
//
// Known issue: Unexpected data is currently not handled, see the implementation of recv.
//
HCI_ATTR
void hci_dispatch_data(void)
{
  uint8_t rx_data_type = hci_read_u8();
  DEBUG_LV3(SERIAL_PRINTVAR_HEX(rx_data_type));

  uint8_t rx_args_size = hci_read_u8();
  DEBUG_LV3(SERIAL_PRINTVAR(rx_args_size));

  uint16_t rx_payload_size = hci_read_u16_le();
  DEBUG_LV3(SERIAL_PRINTVAR(rx_payload_size));

  for (uint8_t i = 0; i < rx_args_size; i++)
    hci_read_u8();

  hci_data_available = 1;
}

//
// hci_dispatch
//
// Called by the interrupt handler to reads the incoming transmition type and dispatch
// it accordingly.  See hci_dispatch_event and hci_dispatch_data for details.
//
HCI_ATTR
void hci_dispatch(void)
{
  DEBUG_LV3(SERIAL_PRINTFUNCTION());

  uint8_t rx_type = hci_read_u8();
  DEBUG_LV3(SERIAL_PRINTVAR(rx_type));

  if (rx_type == HCI_TYPE_EVNT)
    hci_dispatch_event();
  else if (rx_type == HCI_TYPE_DATA)
    hci_dispatch_data();
}

//
// Interrupt handler
//
// There are two modes for the interrupt handler: waiting for assertion and idle.
//
// Waiting for assertion is a special mode, wherein a function that is about to communicate
// with the CC3000 enables the mode and then asserts the CS.  The response from the CC3000 is
// to raise an interrupt, which we do not want to process.  Instead, we notify the function
// that the expect interrupt has been received by restoring the state to idle.
//
// In idle mode, the interrupt handler reads the event handler and then dispatches it
// according to its type.  See hci_dispatch for more information.
//
HCI_ATTR
void hci_irq(void)
{
  DEBUG_LV3(SERIAL_PRINTFUNCTION());
  if (hci_state == HCI_STATE_WAIT_ASSERT)
  {
    hci_state = HCI_STATE_IDLE;
  }
  else
  {
    hci_begin_receive();
    hci_dispatch();
  }
}

//
// hci_begin_first_command
//
// Sends the headers for a given opcode, with special handling for the first command after
// the CC3000 powers on.
//
HCI_ATTR
void hci_begin_first_command(uint16_t opcode, uint16_t argsSize)
{
  DEBUG_LV3(SERIAL_PRINTFUNCTION());

  // First Host Write Operation

  // 1. The master detects the IRQ line low: in this case the detection of IRQ low does not
  //    indicate the intention of the CC3000 device to communicate with the master but rather
  //    CC3000 readiness after power up.
  uint16_t start = millis();
  while (digitalRead(CC3K_IRQ_PIN) != LOW)
  {
    if (millis() - start >= 5000)
    {
      SERIAL_PRINTLN("Failed to detect CC3000.  Check wiring?");
      for (;;);
    }
    wdt_reset();
  }

  // 2. The master asserts nCS.
  digitalWrite(CC3K_CS_PIN, LOW);

  // 3. The master introduces a delay of at least 50 μs before starting actual transmission of data.
  delay(50);

  // 4. The master transmits the first 4 bytes of the SPI header.
  hci_pad = (argsSize & 1) == 0;
  hci_payload_size = 4 + argsSize + hci_pad;
  hci_transfer(HCI_WRITE);
  hci_transfer(hci_payload_size >> 8);
  hci_transfer(hci_payload_size & 0xff);
  hci_transfer(0);

  // 5. The master introduces a delay of at least an additional 50 μs.
  delay(50);

  // 6. The master transmits the rest of the packet.
  hci_transfer(0);
  hci_transfer(HCI_TYPE_CMND);
  hci_transfer(opcode & 0xff);
  hci_transfer(opcode >> 8);
  hci_transfer(argsSize);
}

//
// hci_begin_command
//
// Sends the headers for a given command opcode.
//
HCI_ATTR
void hci_begin_command(uint16_t opcode, uint16_t argsSize)
{
  DEBUG_LV3(SERIAL_PRINTFUNCTION());

  // Generic Host Write Operation
  hci_state = HCI_STATE_WAIT_ASSERT;

  // 1. The master asserts nCS (that is, drives the signal low) and waits for IRQ assertion.
  digitalWrite(CC3K_CS_PIN, LOW);

  // 2. The CC3000 device asserts IRQ when ready to receive the data.
  wdt_reset();
  while (hci_state != HCI_STATE_IDLE)
    ; // intentionally no wdt_reset()

  // 3. The master starts the write transaction. The write transaction consists of a 5-byte header
  //    followed by the payload and a padding byte (if required: remember, the total packet length
  //    must be 16-bit aligned).
  hci_pad = (argsSize & 1) == 0;
  hci_payload_size = 4 + argsSize + hci_pad;
  hci_transfer(HCI_WRITE);
  hci_transfer(hci_payload_size >> 8);
  hci_transfer(hci_payload_size & 0xff);
  hci_transfer(0);
  hci_transfer(0);

  hci_transfer(HCI_TYPE_CMND);
  hci_transfer(opcode & 0xff);
  hci_transfer(opcode >> 8);
  hci_transfer(argsSize);
}

//
// hci_end_command_begin_receive
//
// Finishes sending a command and also prepares to wait for its response event.
//
// These are done as a functional unit to ensure we are prepared for the event interrupt
// before we finalize sending the command.
//
HCI_ATTR
void hci_end_command_begin_receive(uint16_t event, uint32_t timeout)
{
  hci_pending_event = event;
  hci_pending_event_available = 0;
  hci_data_available = 0;
  uint32_t tmr = millis() + timeout;

  if (hci_pad)
    hci_transfer(0);

  // 4. After the last byte of data, the nCS is deasserted by the master.
  digitalWrite(CC3K_CS_PIN, HIGH);

  // 5. The CC3000 device deasserts the IRQ line.

  wdt_reset();
  while (!hci_pending_event_available && millis() <= tmr)
    ; // intentionally no wdt_reset().
  if (tmr < millis()) {
    wifi_callback(HCI_EVNT_CC3000_LOCKED, 0);
  }
}

//
// hci_begin_data
//
// Sends the headers for a given data opcode.
//
HCI_ATTR
void hci_begin_data(uint16_t opcode, uint8_t argsSize, uint16_t bufferSize)
{
  DEBUG_LV3(SERIAL_PRINTFUNCTION());

  // Generic Host Write Operation

  // 1. The master asserts nCS (that is, drives the signal low) and waits for IRQ assertion.
  hci_state = HCI_STATE_WAIT_ASSERT;

  digitalWrite(CC3K_CS_PIN, LOW);

  // 2. The CC3000 device asserts IRQ when ready to receive the data.
  wdt_reset();
  while (hci_state != HCI_STATE_IDLE)
    ; // intentionally no wdt_reset().

  // 3. The master starts the write transaction. The write transaction consists of a 5-byte header
  //    followed by the payload and a padding byte (if required: remember, the total packet length
  //    must be 16-bit aligned).
  int totalSize = argsSize + bufferSize;
  hci_pad = (totalSize & 1) != 0;
  hci_payload_size = 4 + totalSize + hci_pad;
  hci_transfer(HCI_WRITE);
  hci_transfer(hci_payload_size >> 8);
  hci_transfer(hci_payload_size);
  hci_transfer(0);
  hci_transfer(0);

  hci_transfer(HCI_TYPE_DATA);
  hci_transfer(opcode);
  hci_transfer(argsSize);
  hci_transfer(totalSize);
  hci_transfer(totalSize >> 8);
}

#define hci_end_data_begin_receive hci_end_command_begin_receive

//
// hci_wait_data
//
// Waits for the interrupt handler to begin receiving an expected data message.
// See recv for known issues regarding client drops.
//
HCI_ATTR
void hci_wait_data(void)
{
  DEBUG_LV3(SERIAL_PRINTFUNCTION());

  wdt_reset();
  while (!hci_data_available)
    ; // intentionally no wdt_reset()
}

//
// hci_read_status
//
// Reads the status byte which is a common feature in most event messages.
// As the status byte is not returned via the API, it is discarded.
//
HCI_ATTR
void hci_read_status(void)
{
  int status = hci_read_u8();
  DEBUG_LV2(SERIAL_PRINTVAR(status));
}

//
// hci_end_command_receive_u32_result
//
// Implements a common pattern for retrieving the results of commands.
//
HCI_ATTR
uint32_t hci_end_command_receive_u32_result(uint16_t event, uint32_t timeout)
{
  hci_end_command_begin_receive(event, timeout);

  hci_read_status();

  uint32_t result = hci_read_u32_le();
  DEBUG_LV2(SERIAL_PRINTVAR(result));

  hci_end_receive();

  return result;
}

void wlan_init(void)
{
  DEBUG_LV2(SERIAL_PRINTFUNCTION());

#if USE_WATCHDOG
  cli();  // disable all interrupts
  wdt_reset(); // reset the WDT timer
  MCUSR &= ~(1<<WDRF);  // because the data sheet said to
  /*
  WDTCSR configuration:
  WDIE = 1 :Interrupt Enable
  WDE = 1  :Reset Enable - I won't be using this on the 2560
  WDP3 = 0 :For 1000ms Time-out
  WDP2 = 1 :bit pattern is
  WDP1 = 1 :0110  change this for a different
  WDP0 = 0 :timeout period.
  */
  // Enter Watchdog Configuration mode:
  WDTCSR = (1<<WDCE) | (1<<WDE);
  // Set Watchdog settings: interrupte enable, 0110 for timer
  WDTCSR = (1<<WDIE) | (0<<WDP3) | (1<<WDP2) | (1<<WDP1) | (0<<WDP0);
  sei();
  SERIAL_PRINTLN("Finished watchdog setup");  // just here for testing
#endif

  pinMode(CC3K_EN_PIN, OUTPUT);
  digitalWrite(CC3K_EN_PIN, LOW);
  delay(500);

  pinMode(CC3K_CS_PIN, OUTPUT);
  pinMode(CC3K_IRQ_PIN, INPUT_PULLUP);

  digitalWrite(CC3K_CS_PIN, HIGH);
  digitalWrite(CC3K_EN_PIN, HIGH);
  delay(100);

  SPI.begin();
  SPI.setDataMode(SPI_MODE1);
  SPI.setBitOrder(MSBFIRST);
  SPI.setClockDivider(SPI_CLOCK_DIV2);
  delay(100);

  hci_begin_first_command(HCI_CMND_SIMPLE_LINK_START, 1);
  hci_write_u8(SL_PATCHES_REQUEST_DEFAULT);
  attachInterrupt(CC3K_IRQ_NUM, hci_irq, FALLING);
  hci_end_command_begin_receive(HCI_CMND_SIMPLE_LINK_START, 1000);
  hci_end_receive();

  hci_begin_command(HCI_CMND_READ_BUFFER_SIZE, 0);
  hci_end_command_begin_receive(HCI_CMND_READ_BUFFER_SIZE, 1000);
  hci_read_status();
  hci_buffer_count = hci_read_u8();
  hci_available_buffer_count = hci_buffer_count;
  DEBUG_LV2(SERIAL_PRINTVAR(hci_buffer_count));
  hci_buffer_size = hci_read_u16_le();
  DEBUG_LV2(SERIAL_PRINTVAR(hci_buffer_size));
  hci_end_receive();

  hci_begin_command(HCI_CMND_EVENT_MASK, 4);
  hci_write_u32_le(HCI_EVNT_WLAN_KEEPALIVE | HCI_EVNT_WLAN_UNSOL_INIT);
  hci_end_command_begin_receive(HCI_CMND_EVENT_MASK, 1000);
  hci_end_receive();
}

#define MIN_TIMER_VAL_SECONDS      20
#define MIN_TIMER_SET(t)    if ((0 != t) && (t < MIN_TIMER_VAL_SECONDS)) \
                            { \
                                t = MIN_TIMER_VAL_SECONDS; \
                            }

long netapp_timeout_values(unsigned long *aucDHCP, unsigned long *aucARP, unsigned long *aucKeepalive, unsigned long *aucInactivity)
{
  DEBUG_LV2(
    SERIAL_PRINTFUNCTION();
    SERIAL_PRINTVAR(*aucDHCP);
    SERIAL_PRINTVAR(*aucARP);
    SERIAL_PRINTVAR(*aucKeepalive);
    SERIAL_PRINTVAR(*aucInactivity);
    )

  MIN_TIMER_SET(*aucDHCP)
  MIN_TIMER_SET(*aucARP)
  MIN_TIMER_SET(*aucKeepalive)
  MIN_TIMER_SET(*aucInactivity)

  hci_begin_command(HCI_NETAPP_SET_TIMERS, 16);
  hci_write_u32_le(*aucDHCP);
  hci_write_u32_le(*aucARP);
  hci_write_u32_le(*aucKeepalive);
  hci_write_u32_le(*aucInactivity);

  return hci_end_command_receive_u32_result(HCI_NETAPP_SET_TIMERS, 1000);
}

int32_t wlan_ioctl_set_connection_policy(bool should_connect_to_open_ap, bool should_use_fast_connect, bool use_profiles)
{
  DEBUG_LV2(
    SERIAL_PRINTFUNCTION();
    SERIAL_PRINTVAR(should_connect_to_open_ap);
    SERIAL_PRINTVAR(should_use_fast_connect);
    SERIAL_PRINTVAR(use_profiles);
    )

  hci_begin_command(HCI_CMND_WLAN_IOCTL_SET_CONNECTION_POLICY, 12);
  hci_write_u32_le(should_connect_to_open_ap);
  hci_write_u32_le(should_use_fast_connect);
  hci_write_u32_le(use_profiles);

  return hci_end_command_receive_u32_result(HCI_CMND_WLAN_IOCTL_SET_CONNECTION_POLICY, 1000);
}

int32_t wlan_connect(unsigned long sec_type, const char *ssid, long ssid_len, unsigned char *bssid, unsigned char *key, long key_len)
{
  DEBUG_LV2(
    SERIAL_PRINTFUNCTION();
    SERIAL_PRINTVAR(sec_type);
    SERIAL_PRINTVAR(ssid);
    SERIAL_PRINTVAR(ssid_len);
    SERIAL_PRINTVAR((char*)bssid);
    SERIAL_PRINTVAR((char*)key);
    SERIAL_PRINTVAR(key_len);
    )

  static unsigned char bssid_zero[6] = {0, 0, 0, 0, 0, 0};

  hci_begin_command(HCI_CMND_WLAN_CONNECT, 28 + ssid_len + key_len);
  hci_write_u32_le(0x1c);
  hci_write_u32_le(ssid_len);
  hci_write_u32_le(sec_type);
  hci_write_u32_le(16 + ssid_len);
  hci_write_u32_le(key_len);
  hci_write_u16_le(0);
  if (bssid)
    hci_write_array(bssid, 6);
  else
    hci_write_array(bssid_zero, 6);
  hci_write_array(ssid, ssid_len);
  if (key_len && key)
    hci_write_array(key, key_len);

  return hci_end_command_receive_u32_result(HCI_CMND_WLAN_CONNECT, 60000);
}

int setsockopt(long sd, long level, long optname, const void *optval, unsigned long optlen)
{
  DEBUG_LV2(
    SERIAL_PRINTFUNCTION();
    SERIAL_PRINTVAR(sd);
    SERIAL_PRINTVAR(level);
    SERIAL_PRINTVAR(optname);
    SERIAL_PRINTVAR_HEX((int)optval);
    SERIAL_PRINTVAR(optlen);
    )

  hci_begin_command(HCI_CMND_SETSOCKOPT, 20 + optlen);
  hci_write_u32_le(sd);
  hci_write_u32_le(level);
  hci_write_u32_le(optname);
  hci_write_u32_le(8);
  hci_write_u32_le(optlen);
  hci_write_array(optval, optlen);

  return hci_end_command_receive_u32_result(HCI_CMND_SETSOCKOPT, 1000);
}

int socket(long domain, long type, long protocol)
{
  DEBUG_LV2(
    SERIAL_PRINTFUNCTION();
    SERIAL_PRINTVAR(domain);
    SERIAL_PRINTVAR(type);
    SERIAL_PRINTVAR(protocol);
    )

  hci_begin_command(HCI_CMND_SOCKET, 12);
  hci_write_u32_le(domain);
  hci_write_u32_le(type);
  hci_write_u32_le(protocol);

  return hci_end_command_receive_u32_result(HCI_CMND_SOCKET, 1000);
}

int listen(int sd, int backlog)
{
  DEBUG_LV2(
    SERIAL_PRINTFUNCTION();
    SERIAL_PRINTVAR(sd);
    SERIAL_PRINTVAR(backlog);
    )

  hci_begin_command(HCI_CMND_LISTEN, 8);
  hci_write_u32_le(sd);
  hci_write_u32_le(backlog);

  return hci_end_command_receive_u32_result(HCI_CMND_LISTEN, 1000);
}

int bind(int sd, struct _sockaddr_t *addr, int addrlen)
{
  DEBUG_LV2(
    SERIAL_PRINTFUNCTION();
    SERIAL_PRINTVAR(sd);
    SERIAL_PRINTVAR(addrlen);
    SERIAL_PRINTVAR(((_sockaddr_in_t*)addr)->sin_family);
    SERIAL_PRINTVAR(((_sockaddr_in_t*)addr)->sin_port);
    SERIAL_PRINTVAR_HEX(((_sockaddr_in_t*)addr)->sin_addr.s_addr);
    )

  hci_begin_command(HCI_CMND_BIND, 20);
  hci_write_u32_le(sd);
  hci_write_u32_le(0x8);
  hci_write_u32_le(addrlen);
  hci_write_array(addr, 8);

  return hci_end_command_receive_u32_result(HCI_CMND_BIND, 1000);
}

int accept(int sd, struct sockaddr_t *addr, unsigned long *addrlen)
{
  DEBUG_LV2(
    SERIAL_PRINTFUNCTION();
    SERIAL_PRINTVAR(sd);
    )

  hci_begin_command(HCI_CMND_ACCEPT, 4);
  hci_write_u32_le(sd);

  hci_end_command_begin_receive(HCI_CMND_ACCEPT, 1000);

  hci_read_status();

  uint32_t return_sd = hci_read_u32_le();
  DEBUG_LV2(SERIAL_PRINTVAR(return_sd));

  int32_t return_status = hci_read_u32_le();
  DEBUG_LV2(SERIAL_PRINTVAR(return_status));

  if (addr)
    hci_read_array((uint8_t*)addr, 8);
  if (addrlen)
    *addrlen = 8;

  hci_end_receive();

  // Return status is actually the socket descriptor.
  if (return_status < 0 || return_status >= 8)
    return -1;

  return return_status;
}

int recv(int sd, void *buffer, int size, int flags)
{
  DEBUG_LV2(
    SERIAL_PRINTFUNCTION();
    SERIAL_PRINTVAR(sd);
    SERIAL_PRINTVAR(size);
    SERIAL_PRINTVAR(flags);
    )

  hci_begin_command(HCI_CMND_RECV, 12);
  hci_write_u32_le(sd);
  hci_write_u32_le(size);
  hci_write_u32_le(flags);

  hci_end_command_begin_receive(HCI_CMND_RECV, 5000);

  hci_read_status();

  long return_sd = hci_read_u32_le();
  DEBUG_LV2(SERIAL_PRINTVAR(return_sd));

  long return_length = hci_read_u32_le();
  DEBUG_LV2(SERIAL_PRINTVAR(return_length));

  long return_flags = hci_read_u32_le();
  DEBUG_LV2(SERIAL_PRINTVAR_HEX(return_flags));

  hci_end_receive();

  if (return_length > 0)
  {
    // TODO: while waiting for data, we need to handle unsolicited client drops
    //  and stop waiting.
    // Also for safety, the interrupt handler should be able to ignore
    //  unsolicited data when we're not waiting.
    // Thus, the intention to wait for data must be indicated to hci_end_receive,
    //  e.g. hci_end_receive_with_data.
    hci_wait_data();

    if (return_length > size)
      return_length = size;

    for (int i = 0; i < return_length; i++)
      ((uint8_t*)buffer)[i] = hci_read_u8();

    hci_end_receive();
  }

  return return_length;
}

int send(int sd, const void *buffer, int size, int flags)
{
  DEBUG_LV2(
    SERIAL_PRINTFUNCTION();
    SERIAL_PRINTVAR(sd);
    SERIAL_PRINTVAR(size);
    SERIAL_PRINTVAR(flags);
    )

  DEBUG_LV3(SERIAL_PRINTVAR(hci_available_buffer_count));
  wdt_reset();
  while (hci_available_buffer_count == 0)
    ; // intentionally no wdt_reset()
  hci_available_buffer_count--;

  hci_begin_data(HCI_CMND_SEND, 16, size);
  hci_write_u32_le(sd);
  hci_write_u32_le(12);
  hci_write_u32_le(size);
  hci_write_u32_le(flags);
  hci_write_array(buffer, size);

  hci_end_data_begin_receive(HCI_EVNT_SEND, 5000);
  hci_end_receive();

  return size;
}

int closesocket(int sd)
{
  DEBUG_LV2(
    SERIAL_PRINTFUNCTION();
    SERIAL_PRINTVAR(sd);
    )

  wdt_reset();
  while (hci_available_buffer_count != hci_buffer_count)
    ; // intentionally no wdt_reset()

  hci_begin_command(HCI_CMND_CLOSE_SOCKET, 4);
  hci_write_u32_le(sd);

  return hci_end_command_receive_u32_result(HCI_CMND_CLOSE_SOCKET, 1000);
}

int select(long nfds, fd_set *readsds, fd_set *writesds, fd_set *exceptsds, timeval *timeout)
{
  DEBUG_LV2(
    SERIAL_PRINTFUNCTION();
    SERIAL_PRINTVAR(nfds);
    )

  hci_begin_command(HCI_CMND_SELECT, 44);
  hci_write_u32_le(nfds);
  hci_write_u32_le(0x14);
  hci_write_u32_le(0x14);
  hci_write_u32_le(0x14);
  hci_write_u32_le(0x14);
  hci_write_u32_le(timeout != NULL);
  hci_write_u32_le(((readsds) ? *(unsigned long*)readsds : 0));
  hci_write_u32_le(((writesds) ? *(unsigned long*)writesds : 0));
  hci_write_u32_le(((exceptsds) ? *(unsigned long*)exceptsds : 0));
  if (timeout)
  {
    if (timeout->tv_sec == 0 && timeout->tv_usec < 5000)
      timeout->tv_usec = 5000;
    hci_write_u32_le(timeout->tv_sec);
    hci_write_u32_le(timeout->tv_usec);
  }
  else
  {
    hci_write_u32_le(0);
    hci_write_u32_le(0);
  }

  hci_end_command_begin_receive(HCI_CMND_SELECT, 10000);

  hci_read_status();

  int32_t return_status = hci_read_u32_le();
  DEBUG_LV2(SERIAL_PRINTVAR(return_status));

  uint32_t rdfd = hci_read_u32_le();
  DEBUG_LV2(SERIAL_PRINTVAR(rdfd));
  if (readsds) *(uint32_t*)readsds = rdfd;

  uint32_t wrfd = hci_read_u32_le();
  DEBUG_LV2(SERIAL_PRINTVAR(wrfd));
  if (writesds) *(uint32_t*)writesds = wrfd;

  uint32_t exfd = hci_read_u32_le();
  DEBUG_LV2(SERIAL_PRINTVAR(exfd));
  if (exceptsds) *(uint32_t*)exceptsds = exfd;

  hci_end_receive();

  return return_status;
}

int connect(int sd, const sockaddr *addr, long addrlen)
{
  DEBUG_LV2(
    SERIAL_PRINTFUNCTION();
    SERIAL_PRINTVAR(sd);
    SERIAL_PRINTVAR(addr);
    SERIAL_PRINTVAR(addrlen);
    )

  if (!addr || !addrlen)
    return EFAIL;

  /* Override the user */
  addrlen = 8;

  // Send connect command
  hci_begin_command(HCI_CMND_CONNECT, 12 + addrlen);
  hci_write_u32_le(sd);
  hci_write_u32_le(0x08);
  hci_write_u32_le(addrlen);
  hci_write_array(addr, addrlen);

  return hci_end_command_receive_u32_result(HCI_CMND_CONNECT, 10000);
}

int gethostbyname(char *hostname, unsigned short hnLength, uint32_t *ip)
{
  DEBUG_LV2(
    SERIAL_PRINTFUNCTION();
    SERIAL_PRINTVAR(hostname);
    SERIAL_PRINTVAR(hnLength);
    SERIAL_PRINTVAR(ip);
    )

  // Sanity checks
  if (!hostname || !hnLength || hnLength > HOSTNAME_MAX_LENGTH)
    return EFAIL;

  // Send command
  hci_begin_command(HCI_CMND_GETHOSTNAME , 8 + hnLength);
  hci_write_u32_le(0x08);
  hci_write_u32_le(hnLength);
  hci_write_array(hostname, hnLength);
  hci_end_command_begin_receive(HCI_CMND_GETHOSTNAME, 10000);

  // Get result
  hci_read_status();
  uint32_t return_status = hci_read_u32_le();
  *ip = hci_read_u32_le();
  hci_end_receive();

  return return_status;
}

//
// From CC3000 fw version 1.32 and up the module does not longer
// support the mdnsAdvertise command. Instead this has to be
// implemented in software. This is why we check here for the fw
// version.
#if CC3000_FW_VERSION < 132
int mdnsAdvertiser(unsigned short mdnsEnabled, char *deviceServiceName, unsigned short deviceServiceNameLength)
{
  DEBUG_LV2(
    SERIAL_PRINTFUNCTION();
    SERIAL_PRINTVAR(mdnsEnabled);
    SERIAL_PRINTVAR(deviceServiceName);
    SERIAL_PRINTVAR(deviceServiceNameLength);
    )

  if (deviceServiceNameLength > MDNS_DEVICE_SERVICE_MAX_LENGTH)
    return -1;

  hci_begin_command(HCI_CMND_MDNS_ADVERTISE, 12 + deviceServiceNameLength);
  hci_write_u32_le(mdnsEnabled);
  hci_write_u32_le(8);
  hci_write_u32_le(deviceServiceNameLength);
  hci_write_array(deviceServiceName, deviceServiceNameLength);
  return hci_end_command_receive_u32_result(HCI_CMND_MDNS_ADVERTISE, 5000);
}
#endif
