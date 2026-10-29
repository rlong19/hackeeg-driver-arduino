# HackEEG Arduino Driver

This is the Arduino driver code for the [HackEEG Arduino Due shield](https://github.com/starcat-io/hackeeg-shield) for the [TI ADS1299](http://www.ti.com/product/ads1299) EEG system-on-a-chip.
  
The [TI ADS1299](http://www.ti.com/product/ads1299) is a 24-bit 8-channel ADC meant specifically for EEG, with 24x programmable gain amplifiers and much of the analog circuitry needed for EEG. It is capable of digitizing 16,000 samples per second at 24 bit resolution. The ADS1299-4 is a 4-channel version of the ADS1299; the ADS1299-6 is a 6-channel version.

## Arduino drivers

The src/ directory contains an Arduino sketch and associated C/C++ files that make up a driver for ADS129x chips. So far it has only been tested on the ADS1299, but should work on the other models. This driver has been tested on the Arduino Due and Mega2560, but should also work on other Arduinos. The DMA mode can only be used on the Arduino Due.

The driver has a text-mode interface, so can be used without any client software – just open up a serial port to the SAM3X8E native USB port (line endings NL+CR). It also has a JSONLines mode for easy parsing by client programs and a MessagePack mode for efficient binary communication.

The same sketch builds for the original ESP32 (ESP32-WROOM-32 on an ESP32-DevKitC or a similar board). USB serial keeps this HackEEG protocol. WiFi and classic Bluetooth SPP speak the OpenBCI Cyton protocol so BrainFlow and the OpenBCI GUI can connect. See [ESP32 WiFi and Bluetooth](#esp32-wifi-and-bluetooth). 

In MessagePack mode, using the Arduino Due's native SPI DMA, driver can read from the ADS1299 at 16,000 samples per second, and can send that data on to the host via the Arduino Due's USB 2.0 High Speed connection at the same rate. 

By default the driver uses the Arduino library's software SPI (without DMA), and can read and send 8,000 samples per second in that configuration on the Arduino Due, either in JSON Lines mode or MessagePack mode. Other Arduinos will have lower performance.

When in MessagePack mode, MessagePack format is only used to transfer data in `rdata` and `rdatac` commands; all other communication takes place by JSON Lines.

In text mode, samples are encoded using the [base64](http://en.wikipedia.org/wiki/Base64) encoding by default.


## Installing

You must install [ArduinoJson 6](https://arduinojson.org/v6/doc/installation/) before compiling this driver (6.21.5 is known to build). ArduinoJson 7 uses a different API and will not compile this sketch.


## Using the Driver

The commands that are available are:

* RREG – read register. Takes two hex digits as an argument (one byte, for example: FF). Argument 1 is the register to read.
Returns a single hex-encoded byte (for example, 0E) that represents the contents of the register.
* WREG – write register. Takes two hex-encoded bytes as arguments, separated by a space. Argument 1 is the register to write, argument 2 is the register value.
* RDATA – read one sample of data from the ADS129x chip. Returns 3 bytes of header, plus 3 bytes x number of channels (8 for ADS1298 or ADS1299), encoded using base64 or packed hex in text mode, and JSON Lines byte array, or MessagePack byte array, depending on the protocol
* RDATAC – start read data continuous mode. Data is read into a buffer in the Arduino RAM, and streamed to the client one sample at a time, either in packed base64 format or packed hex format in text, JSON Lines byte array, or MessagePack byte array, depending on the protocol.
* SDATAC – stop read data continuous mode. 
* VERSION – reports the driver version
* SERIALNUMBER – reports the HackEEG serial number (UUID from the onboard 24AA256UID-I/SN I2S EEPROM)
* LEDON – turns on the Arduino Due onboard LED.
* LEDOFF – turns off the Arduino Due onboard LED.
* BOARDLEDON – turns on the HackEEG Shield LED. (HackEEG shield has a blue LED connected to ADS1299 GPIO4) 
* BOARDLEDOFF – turns off the HackEEG Shield LED. 
* BASE64 – RDATA/RDATAC commands will encode data in base64.
* TEXT – communication protocol switches to text. See the Communication Protocol section.
* JSONLINES – communication protocol switches from text to [JSONLines](http://jsonlines.org/). This is a text-oriented serialization format with libraries in many languages. See the Communication Protocol section.
* MESSAGEPACK – communication protocol switches from text to [MessagePack](https://msgpack.org) for `rdatac` data only. This is a concise binary serialization format with libraries in many languages. See the Communication Protocol section.
* HEX – RDATA commands will encode data in hexidecimal format.
* HELP – prints a list of available commands.
* On every board, text mode also accepts the analog-front-end commands documented under [HackEEG pipe commands](#hackeeg-pipe-commands) (`srb1`, `bias`, `loff`, `testsignal`, `gain`, `samplerate`, `channels`, `defaults`, `dropped`, and the rest). JSON mode still uses `rreg` / `wreg` for those registers.



## General Operation

The ADS129x chips are configured by reading and writing registers. See the chip datasheet for more information about configuring the ADS129x and reading data from it.

If the host program (the program that reads data from the driver) does not pull data from the serial or USB interface fast enough, the driver will block on sending when the serial or USB buffers fill up. This will cause the driver to lose samples. 

The driver uses the Arduino Native port for serial communication, because it is capable of 2 megabits per second or more.


In most applications, the Python 3 usage will go something like this:

```python
#!/usr/bin/env python

SERIAL_PORT_PATH="/dev/cu.usbmodem14434401"  # your actual path to the Arduino Native serial port device goes here
import sys
import hackeeg
from hackeeg import ads1299

hackeeg = hackeeg.HackEEGBoard(SERIAL_PORT_PATH)
hackeeg.connect()
hackeeg.sdatac()
hackeeg.reset()
hackeeg.blink_board_led()
hackeeg.disable_all_channels()
sample_mode = ads1299.HIGH_RES_250_SPS | ads1299.CONFIG1_const
hackeeg.wreg(ads1299.CONFIG1, sample_mode)
test_signal_mode = ads1299.INT_TEST_4HZ | ads1299.CONFIG2_const
hackeeg.wreg(ads1299.CONFIG2, test_signal_mode)
hackeeg.enable_channel(7)
hackeeg.wreg(ads1299.CH7SET, ads1299.TEST_SIGNAL | ads1299.GAIN_1X)
hackeeg.rreg(ads1299.CH5SET)

# Unipolar mode - setting SRB1 bit sends mid-supply voltage to the N inputs
hackeeg.wreg(ads1299.MISC1, ads1299.SRB1)
# add channels into bias generation
hackeeg.wreg(ads1299.BIAS_SENSP, ads1299.BIAS8P)
hackeeg.rdatac()
hackeeg.start()

while True:
    result = hackeeg.read_response()
    status_code = result.get('STATUS_CODE')
    status_text = result.get('STATUS_TEXT')
    data = result.get(hackeeg.DataKey)
    if data:
        decoded_data = result.get(hackeeg.DecodedDataKey)
        if decoded_data:
            timestamp = decoded_data.get('timestamp')
            ads_gpio = decoded_data.get('ads_gpio')
            loff_statp = decoded_data.get('loff_statp')
            loff_statn = decoded_data.get('loff_statn')
            channel_data = decoded_data.get('channel_data')
            print(f"timestamp:{timestamp} | gpio:{ads_gpio} loff_statp:{loff_statp} loff_statn:{loff_statn} |   ",
                  end='')
            for channel_number, sample in enumerate(channel_data):
                print(f"{channel_number + 1}:{sample} ", end='')
            print()
        else:
            print(data)
        sys.stdout.flush()
```

## SPI DMA 

The driver running on the Arduino communicates with the ADS1299 chip via the SPI interface. For data rates from 250 to 8,192 samples per second, the driver can use the Arduino API's built in software SPI. This is simplest.

To use the 16,384 samples per second data rate, or to reduce CPU load on the Arduino Due, the driver can use the Arduino Due's [Atmel SAM3X8E](https://ww1.microchip.com/downloads/en/DeviceDoc/Atmel-11057-32-bit-Cortex-M3-Microcontroller-SAM3X-SAM3A_Datasheet.pdf) CPU's built-in SPI DMA ([Direct Memory Access](https://en.wikipedia.org/wiki/Direct_memory_access)) controller. Using the SPI DMA offloads all SPI communication to the SAM3X8E DMA controller, freeing up the CPU to do other tasks like serial communication. 

To enable this, change the following constants in `SpiDma.cpp` from this:

Arduino software SPI:

```
#define USE_ARDUINO_SPI_LIBRARY 1
#define USE_NATIVE_SAM3X_SPI 0
```

To this (HackEEG SPI DMA):

```
#define USE_ARDUINO_SPI_LIBRARY 0
#define USE_NATIVE_SAM3X_SPI 1
```

Then recompile and upload the driver sketch to the Arduino.



## Communication Protocol

### Text mode

The driver starts up in this mode. This mode is easiest to use when quick communication with the board is needed without a client program. 

When the command TEXT is given, the driver communication protocol switches to text, and the response will be in text format. Commands can be given in lower or upper case, parameters separated by a space. Responses are given on one line, starting with a status code (200 for OK, errors are in the 300s and 400s.) Commands can be given in upper or lower case.

### JSON Lines mode

Give the JSONLINES command to switch to this mode.

When the command JSONLINES is given, the driver communication protocol switches to [JSON Lines](http://jsonlines.org/) format, and the response will be in JSON Lines format. Commands and Responses are a single map, with keys and values determine the command and parameters. The format is as follows (on separate lines for readability; in use, the entire JSON blob would be on its own line):

#### Commands:

```
{
    COMMAND: "<command-name>",
    PARAMETERS: [ <param1>, <param2>, ... ]
}
```

#### Responses:

```
{
    STATUS_CODE: <status-code>,
    STATUS_TEXT: "<status-text>",
    HEADERS: ["header1", "header2", ... ],
    DATA: [value1, value2, value3, ... ]
}
```

Here is an example exchange:

```
{"COMMAND" : "boardledon"}
{"STATUS_CODE" : 200, "STATUS_TEXT": "Ok" }

```

Headers are optional and may or may not be provided.

#### JSON Lines concise mode for `rdata` and `rdatc` responses

For `rdata` and `rdatac` responses, in order to send data at high speeds, a special response format is used that is similar to the MessagePack format:

```
{
    C: <status-code>,
    D: "<base64-encoded byte-array>"
}
```

In concise JSON Lines mode, status text is omitted. The data is sent as a base64-encoded byte array to minimize transformation of the data into human-readable form. This allows faster transfer. See below for the format of the decoded data byte-array.

#### Software library

The Arduino driver uses the [ArduinoJson](https://arduinojson.org/) library for encoding and decoding JSON Lines data, except for `rdata` and `rdatac` messages– those use a custom, optimized sending routine.


### MessagePack mode

Give the MESSAGEPACK command to switch to this mode.

When the command MESSAGEPACK is given, the driver communication protocol for `rdata` and `rdatac` data packets ONLY switches to [MessagePack](https://msgpack.org) concise binary format. Commands are still given in JSON Lines format, and responses for all commands other than `rdata` and `rdatac` will be in JSON Lines. Responses for `rdata` and `rdatac` will be in MessagePack format. This mode is available to improve data transfer speed since the binary data from the SPI interface can be transferred as-is with no copying or transformation.

The format is as follows (on separate lines as JSON for readability, in use this would be packed as a binary structure):

#### Responses:

```
{
    C: <status-code>,
    D: <byte-array>
}
```

In MessagePack mode, status text is omitted.


#### Software library

The Arduino driver uses the [ArduinoJson](https://arduinojson.org/) library for encoding and decoding MessagePack data, except for `rdata` and `rdatac` messages– those use a custom, optimized sending routine.


### Byte-array Format

The packed byte-array used for `rdata` and `rdatac` transfers has this format:

| position | function      | byte |
|----------|---------------|------|
| 00       | timestamp     |    0 |
| 01       | timestamp     |    1 |
| 02       | timestamp     |    2 |
| 03       | timestamp     |    3 |
| 04       | sample number |    0 |
| 05       | sample number |    1 |
| 06       | sample number |    2 |
| 07       | sample number |    3 |
| 08       | channel 1     |    0 |
| 09       | channel 1     |    1 |
| 10       | channel 1     |    2 |
| 11       | channel 2     |    0 |
| 12       | channel 2     |    1 |
| 13       | channel 2     |    2 |
| 14       | channel 3     |    0 |
| 15       | channel 3     |    1 |
| 16       | channel 3     |    2 |
| 17       | channel 4     |    0 |
| 18       | channel 4     |    1 |
| 19       | channel 4     |    2 |
| 20       | channel 5     |    0 |
| 21       | channel 5     |    1 |
| 22       | channel 5     |    2 |
| 23       | channel 6     |    0 |
| 24       | channel 6     |    1 |
| 25       | channel 6     |    2 |
| 26       | channel 7     |    0 |
| 27       | channel 7     |    1 |
| 28       | channel 7     |    2 |
| 29       | channel 8     |    0 |
| 30       | channel 8     |    1 |
| 31       | channel 8     |    2 |

* `timestamp` is an unsigned long, the result of calling the Arduino's `micros()` function right before the sample is taken.
* `sample number` is an unsigned long, the value is incremented for every sample received by the driver. It is reset every time you issue the `start` command. You can analyze the sample number sequence on the client to see if your client code is dropping or missing samples.
* For an example Python decoding function, see [`_decode_data()`](https://github.com/adamfeuer/ADS129x-tools/blob/master/ads129x_client/hackeeg/driver.py#L123-L152) in the file `hackeeg/driver.py`.


## HackEEG Python Client Software

There is [HackEEG Python client software](https://github.com/starcat-io/hackeeg-client-python) that can run on macOS, Linux, and Windows, and stream data 
to [Lab Streaming Layer](https://github.com/sccn/labstreaminglayer) at 16,000 samples per second.

## Hardware

If you are looking for a full Arduino shield with analog input capability, you might be interested in the 
[HackEEG Shield](https://github.com/adamfeuer/hackeeg-shield). The HackEEG shield is designed for use with the ADS1299.

## ESP32 WiFi and Bluetooth

The sketch `hackeeg_driver/hackeeg_driver.ino` is the ESP32 firmware as well as the Due firmware. On the original ESP32 it keeps the HackEEG text, JSON Lines, and MessagePack protocol on USB serial, and adds the OpenBCI Cyton stream on WiFi and classic Bluetooth SPP.

ESP32-S2, S3, C3, and C6 are not targets. They do not have Bluetooth Classic.

### Wiring

SPI is mode 1, MSB first, SCLK 8 MHz. MOSI drives ADS1299 DIN. MISO reads DOUT. CS is a GPIO, not the SPI peripheral's automatic chip select. DRDY is a falling-edge interrupt. PWDN and RESET are active low. START stays low until a stream starts.

Tie CLKSEL high for the internal 2.048 MHz oscillator, and tie DAISY_IN to DGND when this is the only device. The firmware turns on the internal 4.5 V reference and, on the ESP32, the bias amplifier. It does not build the analog bias network; that has to already be on the board. GPIO 5 is a strapping pin and must be high at reset, which matches CS idle-high. Do not use GPIO 0, 2, 12, or 15 for the ADS1299. GPIO 2 is the DevKit LED.

These are the default GPIOs (change any of them with `-DHACKEEG_PIN_DRDY=34` and the matching `HACKEEG_PIN_*` names):

| ESP32 GPIO | ADS1299 | Notes |
| --- | --- | --- |
| 18 | SCLK | VSPI SCK on the ESP32-DevKitC. On an Adafruit ESP32 Feather V2 this is GPIO 18, not the pin labeled SCK. |
| 23 | DIN | MOSI. |
| 19 | DOUT | MISO. |
| 5 | CS | Active low. Idle high. |
| 27 | DRDY | Falling edge. Digital input; WiFi does not prevent that. |
| 25 | START | Driven low until streaming. |
| 26 | RESET | Active low pulse at boot. |
| 32 | PWDN | Active low. On the Feather V2 this pin is the battery divider; move PWDN to GPIO 33 with `-DHACKEEG_PIN_PWDN=33` on that board. |
| GND | DGND, AGND | Common ground. |
| 3.3 V | DVDD | AVDD / AVSS stay on the analog board. |

### What the ESP32 does at boot

1. Drive PWDN high and hold RESET low for 150 ms (`tPOR` is 128 ms at 2.048 MHz).
2. Release RESET and wait more than 18 tCLK.
3. Send SDATAC before any register access. The part wakes in RDATAC.
4. Read ID. `0x3C` / `0x3D` / `0x3E` are ADS1299-4 / ADS1299-6 / ADS1299 (later revisions with the same channel bits are accepted). The SPI frame is 15, 21, or 27 bytes.
5. If the ID is not recognized, the frame still defaults to 6 channels so an ADS1299-6 can be forced on, and `/board` reports `board_connected` false until the ID is valid or you send `|channels 6|`. `-DHACKEEG_CHANNEL_COUNT=6` (or 4 or 8) forces the frame size at compile time and marks the board connected.
6. Apply a referential default: 250 SPS, gain 24, normal inputs, SRB1 closed, SRB2 open, bias amp on, every present channel in BIAS_SENSP and BIAS_SENSN. The internal reference is given 150 ms to settle.
7. USB then prints diagnostic lines that start with `#`, and finally `Ready`. A HackEEG client should read until the line `Ready`.

Due behavior is unchanged: the same ID decode, the same shield pins, and no OpenBCI default register write. An unrecognized ID still blinks the LED and waits. Teensy uses `Serial` and the control pins CS 10, DRDY 2, START 6, RESET 5, PWDN 3, with hardware SPI on 13/11/12. That pin map is in the sketch; this tree was compile-checked for the ESP32 and the Due.

### OpenBCI packets

WiFi and Bluetooth send the [Cyton 33-byte frame](https://docs.openbci.com/Cyton/CytonDataFormat/):

| Bytes | Contents |
| --- | --- |
| 0 | `0xA0` |
| 1 | Sample number, 0–255, wrapping. Gaps mean a missed DRDY or a dropped packet. |
| 2–25 | Eight 24-bit signed samples, MSB first. Channels the chip does not have, and channels that are powered down, are zero. |
| 26–31 | Zeros. There is no accelerometer, so the GUI accel traces sit at 0. |
| 32 | `0xC0` |

`/board` reports `num_channels` 8 because the packet is 8 wide. `chip_channels` is the ADS1299 width (6 on an ADS1299-6). The GUI scale is `µV = count * (4.5 / 8388607 / gain) * 1e6` with gain 24.

ASCII commands, only while that transport is not streaming binary (a reply would otherwise land in the middle of a packet). HTTP `/command` always returns the text in JSON, including during a stream.

| Command | Effect |
| --- | --- |
| `b` / `s` | Start or stop the binary stream on that transport. |
| `v` | Reset the ADS1299, rewrite the referential defaults, print the banner. |
| `V` | Print `HackEEG v0.4.0$$$`. |
| `?` | Read registers `0x00`–`0x17`, then `$$$`. |
| `xCHPDGAININBIASSRB2SRB1X` | One channel, same character codes as the Cyton SDK. Example: `x1060101X` is channel 1, power on, gain 24, normal input, bias on, SRB2 off, SRB1 on. |
| `1`–`8` and `!@#$%^&*` | Power a channel down or up. Power-down also shorts that input. |
| `d` / `D` | Restore the referential default, or report it as `060101$$$`. |
| `~0` … `~6` or `~~` | Set or query the data rate. `0` = 16 kSPS … `6` = 250 SPS. `~4` is 1000 SPS. |
| `z(channel)(P)(N)Z` | Lead-off sense for one channel, then `$$$`. |
| `0` `-` `=` `[` `]` `p` | Internal test signal: ground, slow 1x, fast 1x, slow 2x, fast 2x, DC. |
| `|...|` | HackEEG extension. See below. |

`c` acknowledges the 8-channel packet. `C` (daisy) and the SD-card letters are consumed and refused so the parser stays aligned. `{` `}` `:` `;` are the Cyton WiFi-shield attach commands and are ignored. Startup text on Bluetooth ends with `$$$`.

### HackEEG pipe commands

OpenBCI commands are single bytes, or a short framed sequence (`x`…`X`, `z`…`Z`, `~` plus one character). A second protocol has to use a character OpenBCI does not use, and must not interpret the bytes inside the frame as `b`, `s`, `x`, or `~`.

The extension frame is a pair of pipe characters:

```text
|rreg 00|
|wreg 03 EC|
|srb1 on|
|samplerate 1000|
```

`|` is not a Cyton command. Bytes between the pipes are the HackEEG command and are not parsed as OpenBCI. A newline also closes the frame, so `|status` and Enter works from a terminal. Commands are case-insensitive. The reply, when the transport is idle, is one line `|200 ...|` or `|400 ...|`. HTTP `/command` puts that same text in the JSON body.

Send it as the Cyton command string. WiFi:

```json
{"command":"|wreg 05 60|"}
```

TCP (the socket the board opened to your computer) and Bluetooth SPP take the same bytes. Do not echo the binary sample stream back into that socket; sample bytes would be parsed as commands.

While that transport is streaming raw packets, text replies are not written onto it. The command still runs. Use HTTP `/command` to read the reply during a stream. `/stream/start` and `b` do not prefix the TCP stream with an acknowledgement, so the first byte BrainFlow sees is `0xA0`.

`|start|` and `|stop|` are the ADS1299 SPI opcodes, same as the USB `start` and `stop` commands. They do not by themselves turn the OpenBCI packet stream on or off. `|rdatac|` and `|sdatac|`, and OpenBCI `b` / `s`, do that. `|rdatac|` from Bluetooth arms Bluetooth only; `|rdatac|` from HTTP or TCP arms WiFi only; USB `rdatac` arms the HackEEG USB stream only. One conversion feeds every stream that is armed.

| Command | Arguments | Effect |
| --- | --- | --- |
| `help` | | List the extension commands. |
| `version` `status` `micros` `serialnumber` `nop` | | Same ideas as the USB commands. `serialnumber` is still unimplemented. |
| `rreg` | hex register | Read one register. Reply is the byte in hex. |
| `wreg` | hex register, hex value | Write one register. Register access during a stream sends SDATAC, writes, then RDATAC. |
| `rdata` | | One SPI frame, hex, when not streaming. |
| `rdatac` `sdatac` | | Arm or disarm the OpenBCI stream on the transport that sent it. |
| `start` `stop` `wakeup` `standby` `reset` | | ADS1299 opcodes. `reset` also rewrites the referential defaults. |
| `channels` | `4`, `6`, or `8` | SPI frame size. Omit the number to query. This stops and restarts an active stream. |
| `samplerate` | `250` `500` `1000` `2000` `4000` `8000` `16000` | CONFIG1 data rate. Omit the number to query. |
| `gain` | channel, `1\|2\|4\|6\|8\|12\|24` | PGA gain. |
| `power` | channel, `on\|off` | Channel power. Off also shorts the input. |
| `input` | channel, `normal\|shorted\|bias\|mvdd\|temp\|test\|bias_drp\|bias_drn` | CHnSET mux. |
| `srb1` | `on\|off` | MISC1 SRB1 (all N inputs). |
| `srb2` | channel or `all`, `on\|off` | Per-channel SRB2 switch. |
| `bias` | `on\|off` | Bias amplifier and internal bias reference. |
| `biasp` `biasn` | hex mask | BIAS_SENSP / BIAS_SENSN. |
| `biasch` | channel, `on\|off` | Include that channel in both bias senses. |
| `loff` | `on\|off` | Lead-off comparators (CONFIG4). |
| `loffp` `loffn` | hex mask | LOFF_SENSP / LOFF_SENSN. |
| `loffch` | channel, P `0\|1`, N `0\|1` | Lead-off sense for one channel. |
| `loff_current` | `6nA` `24nA` `6uA` `24uA` | ADS1299 lead-off magnitude. |
| `loff_freq` | `dc` `7.8` `31.2` `fdr4` | Lead-off frequency. |
| `testsignal` | `off` `dc` `slow` `fast` `slow2` `fast2` `ground` | CONFIG2 test source, and the mux of every present channel. |
| `testamp` | `1` or `2` | Test amplitude `(VREFP-VREFN)/2400` or twice that. |
| `defaults` | | Same referential setup as boot (`d`). |
| `dropped` | | `ring_dropped`, `drdy_missed`, `radio_dropped`, `produced`. |
| `regs` | | Registers `0x00`–`0x17` as `RR=VV` pairs. |
| `led` `boardled` | `on\|off` | ESP32 LED, or ADS1299 GPIO4. |
| `text` `jsonlines` `messagepack` `base64` `hex` | | USB protocol only. |

The same words work on the USB text port without the pipes (`srb1 on`, `wreg 03 EC`).

### WiFi

Leave the station SSID empty and the board starts an access point `HackEEG`, password `openbci123`, at **192.168.4.1**. To join a router, uncomment `HACKEEG_STA_SSID` and `HACKEEG_STA_PASSWORD` in `hackeeg_driver/wifi_secrets.h`, or pass `-DHACKEEG_STA_SSID=\"name\"` and `-DHACKEEG_STA_PASSWORD=\"secret\"`. If the join does not succeed within 12 seconds, the board falls back to the access point. HTTP is port 80.

| Request | Role |
| --- | --- |
| `GET /board` | `board_connected`, `board_type` (`cyton` or `none`), `num_channels` 8, `gains`, plus `chip_channels` and the drop counters. |
| `POST /tcp` | JSON `ip`, `port`, `output` (`raw` or `json`), `delimiter`, `latency`. The ESP32 connects **to that computer** and, once streaming, writes samples. |
| `POST /udp` | Same JSON. One datagram per sample. `"redundancy": true` sends it three times. |
| `POST /command` | `{"command":"~4"}` or `{"command":"|status|"}`. |
| `GET /stream/start` | Start WiFi streaming (`b`). |
| `GET /stream/stop` | Stop WiFi streaming. Bluetooth keeps streaming if it was started separately. |
| `GET /stats` | Drop counters, sample rate, streaming flag. |

`raw` output is concatenated 33-byte packets with no extra delimiter bytes. `delimiter` inserts `\r\n` only in `json` mode. `latency` is accepted and reported; packets are sent on the next loop pass rather than held for that many microseconds. `ip_port` in BrainFlow is a port on the computer, not on the ESP32.

BrainFlow (`CYTON_WIFI_BOARD`):

```python
from brainflow.board_shim import BoardShim, BrainFlowInputParams, BoardIds

params = BrainFlowInputParams()
params.ip_address = "192.168.4.1"  # or the station address printed at boot
params.ip_port = 6677              # free TCP port on this computer
BoardShim.enable_dev_board_logger()
board = BoardShim(BoardIds.CYTON_WIFI_BOARD, params)
board.prepare_session()            # GET /board, POST /tcp, then ~4 (1000 SPS)
board.start_stream()               # GET /stream/start
data = board.get_board_data()
board.stop_stream()
board.release_session()
```

In the OpenBCI GUI choose Cyton over WiFi and enter that IP. `prepare_session` sends `~4`, so a BrainFlow WiFi session runs at 1000 SPS even though the sketch boots at 250. Channels 7 and 8 read as zero on an ADS1299-6.

### Bluetooth

Classic SPP name `HackEEG` (override with `-DHACKEEG_BT_NAME=\"name\"`). Pair, open the serial port, and use the GUI's Cyton (serial / dongle) mode, or BrainFlow `BoardIds.CYTON_BOARD` with `params.serial_port` set to that port. The port speaks `b` / `s` and the pipe commands. There is no PIN. The banner is sent when a client connects and ends with `$$$`.

### Samples, the ring, and dropped counts

DRDY is falling-edge. The interrupt only wakes a high-priority FreeRTOS task; SPI is not done in the interrupt. That task clocks one frame out and pushes a slot into a 512-deep ring. `loop()` drains the ring onto USB (HackEEG format, only after `rdatac`), WiFi, and Bluetooth. A stalled `write` does not have to finish before the next SPI read. When the ring is full the newest sample is counted in `ring_dropped` and is not stored.

`drdy_missed` counts extra DRDY edges that arrived before the previous frame was clocked out. The sample number jumps by that many so the gap is visible in the packet counter. `radio_dropped` counts a packet that WiFi or Bluetooth did not accept. `produced` is how many frames were pushed. Read them with `|dropped|`, USB `status` or `dropped`, or `GET /stats`.

The ring is about 2 seconds at 250 SPS and about 0.5 seconds at 1000 SPS. USB serial on a DevKit is 115200 baud, so the HackEEG JSON/MessagePack stream on USB is the slow path. Use WiFi or Bluetooth for 250–1000 SPS. These rates are from the 33-byte packet size, not a measurement on a board:

| Transport | Rate that should fit |
| --- | --- |
| USB serial at 115200, HackEEG text/JSON | Below 250 SPS once the line is base64 or JSON. |
| Classic SPP | 250 SPS, often 1000 SPS (about 33 KB/s). |
| WiFi TCP | 250 SPS through a few thousand SPS. 1000 SPS is 33 KB/s. |
| WiFi UDP | Same order as TCP. Loss shows up as sample-number gaps. |

### Build

Install [arduino-cli](https://arduino.github.io/arduino-cli/), the ESP32 core, and ArduinoJson 6:

```bash
arduino-cli config init
arduino-cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32
arduino-cli lib install "ArduinoJson@6.21.5"
```

The default 1.2 MB app partition is too small (the binary is about 1.7 MB). Huge APP is 3 MB and has no OTA slot. In the Arduino IDE: board **ESP32 Dev Module**, **Tools → Partition Scheme → Huge APP (3MB No OTA/1MB SPIFFS)**.

```bash
arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=huge_app hackeeg_driver
```

Force a 6-channel frame even if you want the fallback compiled in:

```bash
arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=huge_app \
  --build-property "compiler.cpp.extra_flags=-DHACKEEG_CHANNEL_COUNT=6" \
  hackeeg_driver
```

Checked with arduino-cli, ESP32 core 3.3.12, and ArduinoJson 6.21.5: `esp32:esp32:esp32:PartitionScheme=huge_app` compiles (about 1.69 MB, 53% of Huge APP). The Arduino Due target (`arduino:sam:arduino_due_x`) also compiles. No ADS1299 was attached, so DRDY timing and radio throughput were not measured on hardware.

## Credits

This software would not be possible without the help of many people:

* Kendrick Shaw, Ace Medlock, and Eric Herman (parts of the ADS129x.h header file and some parts of the ADS129x driver, 
* see [OpenHardwareExG project](https://github.com/OpenElectronicsLab/OpenHardwareExG) for more info.
* Chris Rorden (some parts of the ADS1298 driver)
* Stefan Rado (SerialCommand library)
* Steven Cogswell (SerialCommand library)
* William Greiman (SPI DMA library)
* Cristian Maglie (SPI DMA library)
* Bill Porter (SPI DMA library)
* Adam Rudd ([Base64](https://github.com/adamvr/arduino-base64) library)
* Benoît Blanchon ([ArduinoJson](https://arduinojson.org/) library)


If I forgot to credit you, please let me know!

If you have questions, comments, or improvements, I would love to know them! Pull requests welcome!

cheers <br/>
adam <br/>
<br/>
Adam Feuer <br/>
adam@starcat.io<br/>
[Starcat LLC](https://starcat.io)<br/>
Seattle, WA, USA <br/>
