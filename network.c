/******************************************************************************
* File:         network.c
*
* Description:  Code file of the network api for communication between
*               multiple microcontrollers.
*
* Author:       Marius Preuss, Viktor Puselja
*
* Date:         2014-01-21
*
******************************************************************************/

// TODO / Improvements :
// - implement wait_after_collision
// - implement max packet_size option (security)
// - optimize if statements
// - check malloc return value

#include "network.h"

#include <stdlib.h>
#include <string.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/delay.h>

// Volatile Variable
//volatile uint8_t writeMode = false;
//volatile uint8_t readMode = false;
//volatile uint8_t writeModeByte = 0;

struct network_connection network_conn;

volatile uint8_t network_timer_int_mode = NETWORK_TIMER_INTERRUPT_MODE_NONE;

volatile uint8_t network_timer_int_byte = 0;

#define NETWORK_CHECK_IS_INITIALIZED if (!network_conn.is_initialized) \
{ return NETWORK_NOT_INITIALIZED; }

uint8_t network_initialize(void)
{
    uint8_t error = NETWORK_NO_ERROR;
    bool is_address_used = true;
    uint8_t address;

    if (!network_conn.is_initialized) 
    {
        // initialize connection
        network_conn.address = NETWORK_ADDRESS_NONE;
        network_conn.last_id = 0;
        network_conn.is_packet_avaiable = false;
        network_conn.is_initialized = true;

        // determine address
        address = NETWORK_ADDRESS_MIN - 1;
        do
        {
            address++;
            is_address_used = network_check(address);
        } while (address <= NETWORK_ADDRESS_MAX && is_address_used);

        if (!is_address_used) 
        {
            network_conn.address = address;
        } 
        else 
        {
            network_conn.is_initialized = false;
            error = NETWORK_NONE_ADDRESS_AVAILABLE;
        }
    }
    return error;
}

bool network_check(uint8_t address)
{
    uint8_t error = NETWORK_NO_ERROR;
    struct network_packet_header packet_header;

    // send check
    error = network_send(address, NETWORK_STATUS_CHECK, NETWORK_COMMAND_NONE,
        NULL, 0);
    if (error == NETWORK_NO_ERROR) 
    {
        // wait for check acknowledge
        error = network_wait_for_packet(address, NETWORK_STATUS_ACKNOWLEDGE,
            NETWORK_COMMAND_NONE, NETWORK_ID_ANY, &packet_header, NULL,
            NETWORK_TIMEOUT_CHECK);
    }

    return error != NETWORK_TIMEOUT_EXCEEDED;
}

uint8_t network_get_address()
{
    NETWORK_CHECK_IS_INITIALIZED
    return network_conn.address;
}

uint8_t network_get_response(uint8_t address, uint8_t command,
    const uint8_t *request_data, uint8_t request_length,
    uint8_t **response_data, uint8_t *response_length, int16_t timeout)
{
    uint8_t error = NETWORK_NO_ERROR;
    struct network_packet_header response_packet;

    NETWORK_CHECK_IS_INITIALIZED

    // send request
    error = network_send(address, NETWORK_STATUS_REQUEST, command,
        request_data, request_length);
    if (error == NETWORK_NO_ERROR && address != NETWORK_ADDRESS_BROADCAST) 
    {
        // receive response
        error = network_wait_for_packet(address, NETWORK_STATUS_RESPONSE,
            command, NETWORK_ID_ANY, &response_packet, response_data, timeout);
        if (error == NETWORK_NO_ERROR && response_data != NULL)
        {
            *response_length = response_packet.length;
        }
    }

    return error;
}

uint8_t network_get_request(struct network_request_data *request,
    int16_t timeout)
{
    uint8_t error = NETWORK_NO_ERROR;
    struct network_packet_header request_packet;

    NETWORK_CHECK_IS_INITIALIZED

    // receive request
    error = network_wait_for_packet(NETWORK_ADDRESS_NONE,
        NETWORK_STATUS_REQUEST, NETWORK_COMMAND_NONE, NETWORK_ID_ANY,
        &request_packet, &request->data, timeout);
    if (error == NETWORK_NO_ERROR) 
    {
        request->source = request_packet.source;
        request->destination = request_packet.destination;
        request->command = request_packet.command;
        request->length = request_packet.length;
    }

    return error;
}

uint8_t network_send_response(const struct network_request_data *request,
    const uint8_t *data, uint8_t length)
{
    NETWORK_CHECK_IS_INITIALIZED
    // send response
    return network_send(request->source, NETWORK_STATUS_RESPONSE,
        request->command, data, length);
}

uint8_t network_send(uint8_t destination, uint8_t status, uint8_t command,
    const uint8_t *data, uint8_t length)
{
    uint8_t error = NETWORK_NO_ERROR;
    struct network_packet_header packet_to_send;
    uint8_t attempts = 0;

    NETWORK_CHECK_IS_INITIALIZED

    // build packet
    packet_to_send.destination = destination;
    packet_to_send.source = network_conn.address;
    packet_to_send.status = status;
    packet_to_send.command = command;
    packet_to_send.id = ++network_conn.last_id;
    packet_to_send.length = length;

    // send
    do 
    {
        error = network_write_packet(&packet_to_send, data);
        if (error == NETWORK_NO_ERROR)
        {
            error = network_wait_for_acknowledge(&packet_to_send);
        }
        attempts++;
    } while (error == NETWORK_TIMEOUT_EXCEEDED
        && attempts < NETWORK_SEND_ATTEMPTS);
    return error;
}

uint8_t network_wait_for_acknowledge(
    const struct network_packet_header *packet)
{
    uint8_t error = NETWORK_NO_ERROR;
    struct network_packet_header acknowledge_packet;

    NETWORK_CHECK_IS_INITIALIZED

    // check and acknowledge packets must not be awaited for acknowledge
    if (packet->status != NETWORK_STATUS_CHECK
        && packet->status != NETWORK_STATUS_ACKNOWLEDGE
        /*&& packet->status != NETWORK_STATUS_DATA_TO_BIG*/) {
        error = network_wait_for_packet(packet->destination,
            NETWORK_STATUS_ACKNOWLEDGE, packet->command, packet->id,
            &acknowledge_packet, NULL, NETWORK_TIMEOUT_ACKNOWLEDGE);
    /*
    if (error == NETWORK_NO_ERROR)
    {
        if (acknowledge_packet.Status == NETWORK_STATUS_DATA_TO_BIG)
        {
            error = NETWORK_DATA_TO_BIG;
        }
    }
    */
  }
  return error;
}

uint8_t network_wait_for_packet(uint8_t source, uint8_t status,
    uint8_t command, uint8_t id, struct network_packet_header *packet_header,
    uint8_t **packet_data, int16_t timeout)
{
    uint8_t error = NETWORK_NO_ERROR;

    NETWORK_CHECK_IS_INITIALIZED

    do 
    {
        error = network_get_last_packet(packet_header, packet_data);
        if (error == NETWORK_NO_ERROR) 
        {
            // check if it is the desired packet
            if (packet_header != NULL
                && ((source != NETWORK_ADDRESS_NONE
                && source != packet_header->source)
                || (status != NETWORK_STATUS_ANY
                && status != packet_header->status
                /*&& (status == NETWORK_STATUS_ACK_OR_DTB
                && packet_header->status != NETWORK_STATUS_ACKNOWLEDGE
                && packet_header->status != NETWORK_STATUS_DATA_TO_BIG)*/)
                || (command != NETWORK_COMMAND_NONE
                && command != packet_header->command)
                || (id != NETWORK_ID_ANY && id != packet_header->id)))
            {
                error = NETWORK_NO_DATA;
            }
        }
        if (error == NETWORK_NO_DATA) 
        {
            _delay_ms(1);
        }
    } while (error == NETWORK_NO_DATA
        && (timeout == NETWORK_TIMEOUT_INFINITE || timeout-- > 0));
    
    if (error == NETWORK_NO_ERROR)
    {
        network_acknowledge_packet(packet_header);
    }
    else if (timeout < 0)
    {
        error = NETWORK_TIMEOUT_EXCEEDED;
    }
    return error;
}

uint8_t network_get_last_packet(struct network_packet_header *packet_header,
    uint8_t **packet_data)
{
    uint8_t error = NETWORK_NO_ERROR;

    NETWORK_CHECK_IS_INITIALIZED

    // copy packet if avaiable
    if (network_conn.is_packet_avaiable) 
    {
        if (packet_header != NULL)
        {
            memcpy(packet_header, &network_conn.last_packet_header,
            sizeof(struct network_packet_header));
            if (packet_data != NULL)
            *packet_data = network_conn.last_packet_data;
        }
        else
        {
            network_free_data(network_conn.last_packet_data);
            network_conn.last_packet_data = NULL;
            network_conn.is_packet_avaiable = false;
        }
    } 
    else 
    {
        error = NETWORK_NO_DATA;
    }
    return error;
}

uint8_t network_acknowledge_packet(const struct network_packet_header *packet)
{
    uint8_t error = NETWORK_NO_ERROR;
    struct network_packet_header acknowledge_packet;

    NETWORK_CHECK_IS_INITIALIZED

    // broadcast and acknowledge packets must not be acknowledged
    if (packet != NULL && packet->source != NETWORK_ADDRESS_BROADCAST
        && packet->status != NETWORK_STATUS_ACKNOWLEDGE) 
    {
        acknowledge_packet.destination = packet->source;
        acknowledge_packet.source = network_conn.address;
        acknowledge_packet.status = NETWORK_STATUS_ACKNOWLEDGE;
        acknowledge_packet.command = packet->command;
        acknowledge_packet.id = packet->id;
        acknowledge_packet.length = 0;
        error = network_write_packet(&acknowledge_packet, NULL);
    }
    return error;
}

uint8_t network_write_packet(struct network_packet_header *packet_header,
    const uint8_t *packet_data)
{
    uint8_t error = NETWORK_NO_ERROR;
    uint8_t attempts = 0;

    packet_header->checksum = network_calculate_checksum(packet_header);

    // write buffer to the network
    do 
    {
        error = network_write_bytes((const uint8_t*)packet_header,
        sizeof(struct network_packet_header));
        if (error == NETWORK_NO_ERROR)
        {
            error = network_write_bytes(packet_data, packet_header->length);
        }

        if (error == NETWORK_COLLISION_DETECTED)
        {
            network_wait_after_collision();
        }
        attempts++;
    } while (error == NETWORK_COLLISION_DETECTED
        && attempts < NETWORK_WRITE_PACKET_ATTEMPTS);
    return error;
}

void network_wait_after_collision(void)
{
	// TODO: implement
	// use address and key as seed for random wait time
}

uint8_t network_process_byte(uint8_t error, uint8_t data)
{
    static uint8_t packet_index = 0;
    static struct network_packet_header packet_header;
    static uint8_t *packet_data = NULL;
    uint8_t bytes_to_skip = 0;
    bool reset = false;

    if (error == NETWORK_NO_ERROR) 
    {
        if (packet_index < sizeof(struct network_packet_header)) 
        {
            // write byte to header
            ((uint8_t*)&packet_header)[packet_index++] = data;

            if (packet_index == 2) 
            {
                // when destination and length read check destination address
                if (packet_header.destination != network_conn.address
                    && packet_header.destination != NETWORK_ADDRESS_BROADCAST)
                {
                    error = NETWORK_INVALID_PACKET;
                }
                /*
                #ifdef NETWORK_MAX_DATA_SIZE
                else if (packet_header.length > NETWORK_MAX_DATA_SIZE) 
                {
                    network_send(packet_header.source,
                        NETWORK_STATUS_DATA_TO_BIG,
                        NETWORK_COMMAND_NONE, NULL, 0);
                    error = NETWORK_INVALID_PACKET;
                }
                #endif
                */
            }
            else if (packet_index == sizeof(struct network_packet_header))
            {
                // when header read check checksum
                if (network_calculate_checksum(&packet_header)
                    != packet_header.checksum)
                {
                    error = NETWORK_INVALID_PACKET;
                }
            }
        }
        else if ((packet_index - sizeof(struct network_packet_header))
            < packet_header.length) 
        {
            if (packet_data == NULL)
            {
                packet_data = (uint8_t*)malloc(packet_header.length);
            }
            // write byte to data
            packet_data[packet_index++ - sizeof(struct network_packet_header)]
                = data;
        }
    }

    if (error == NETWORK_NO_ERROR) 
    {
        if (packet_index == sizeof(struct network_packet_header)
            + packet_header.length) 
        { 
            // packet read
            if (packet_header.status == NETWORK_STATUS_CHECK) 
            {
                // respond to check
                network_send(packet_header.source, NETWORK_STATUS_ACKNOWLEDGE,
                    NETWORK_COMMAND_NONE, NULL, 0);
            } 
            else if (!network_conn.is_packet_avaiable) 
            {
                memcpy(&network_conn.last_packet_header, &packet_header,
                    sizeof(struct network_packet_header));
                network_conn.last_packet_data = packet_data;
                packet_data = NULL;
                network_conn.is_packet_avaiable = true;
            }
            reset = true;
        }
    }
    else 
    {
        bytes_to_skip = sizeof(struct network_packet_header)
            + packet_header.length - packet_index;
        reset = true;
    }

    if (reset)
    {
        packet_index = 0;
    }

    return bytes_to_skip;
}

uint8_t network_calculate_checksum(struct network_packet_header *packet)
{
    uint8_t checksum = 0;
    uint8_t packet_checksum = packet->checksum;
    uint8_t *current;

    packet->checksum = 0;
    for (current = (uint8_t*)packet; current
        < (uint8_t*)packet + sizeof(struct network_packet_header); current++)
    {
        checksum ^= *current;
    }
    packet->checksum = packet_checksum;
    return checksum;
}

void network_free_data(uint8_t *data)
{
    free(data);
}

void network_pull_up()
{
    NETWORK_SEND_DDR &= ~(1 << NETWORK_SEND_DB);
    NETWORK_SEND_PORT |= (1 << NETWORK_SEND_DB);
}

void network_pull_down()
{
    NETWORK_SEND_PORT &= ~(1 << NETWORK_SEND_DB);
}

uint8_t network_write_bytes(const uint8_t *data, uint8_t length)
{
    uint8_t error = NETWORK_NO_ERROR;
    const uint8_t *counter;

    for (counter = data; error == NETWORK_NO_ERROR && counter < data + length;
        counter++)
    {
        error = network_write_byte(*data);
    }

    return error;
}

uint8_t network_write_byte(uint8_t byte)
{
    while (network_timer_int_mode != NETWORK_TIMER_INTERRUPT_MODE_NONE);
    
    cli(); // disable interrupts
    if (network_timer_int_mode != NETWORK_TIMER_INTERRUPT_MODE_NONE)
        return NETWORK_WRITE_ERROR;

    //writeModeByte = byte;
    //writeMode = true;
    // INT0_vect deaktivieren
    // TIMER2_OVF_vect triggern
	return NETWORK_NO_ERROR;
}

//uint8_t network_send_zero()
//{
//    uint8_t error = NETWORK_NO_ERROR;
//    network_pull_down();
//    // TODO Ueberpruefung auf SLAM
//    return error;
//}

//uint8_t network_send_one()
//{
//    uint8_t error = NETWORK_NO_ERROR;
//    network_pull_up();
//    // TODO Ueberpruefung auf SLAM
//    return error;
//}

uint8_t network_send_bit(uint8_t bit)
{
    uint8_t error = NETWORK_NO_ERROR;
    if (bit)
    {
        network_pull_up();
    }
    else
    {
        network_pull_down();
    }
    
    // check for SLAM
    
    return error;
}

ISR(INT0_vect)
{
    // TODO:
    // TCNT0 = 128;       // TIMER2_OVF_vect auf 128 Impulse
    // TIMER2_OVF_vect aktivieren
    // INT0_vect deaktivieren
}

ISR(TIMER2_OVF_vect)
{
    // TCNT0 = 256;       // TIMER2_OVF_vect auf 256 Impulse
    static uint8_t index = 0;
    bool reset = false;
    uint8_t bit = 0;
    
    if (network_timer_int_mode == NETWORK_TIMER_INTERRUPT_MODE_READ)
    {
        // read bit
        network_timer_int_byte <<= 1;
        network_timer_int_byte |= NETWORK_SEND_PORT;
        
        if (++index == 8) // byte read
        {
            network_process_byte(NETWORK_NO_ERROR, network_timer_int_byte);
            reset = true;
        }
    }
    else if (network_timer_int_mode == NETWORK_TIMER_INTERRUPT_MODE_WRITE)
    {
        // send bit
        bit = network_timer_int_byte & 1;
        network_timer_int_byte <<= 1;
        network_send_bit(bit);
        
        if (++index == 8) // byte written
        {
            reset = true;
        }
    }
    
    if (reset)
    {
        network_timer_int_byte = 0;
        index = 0;
        network_timer_int_mode = NETWORK_TIMER_INTERRUPT_MODE_NONE;
        
        // deactivate timer
        // active signal interrupt (INT0_vect aktivieren)
    }

    //if (readMode == true) 
    //{
    //    static uint8_t readModeCounter = 0;
    //    static uint8_t readModeByte = 0;
    //    static uint8_t readModeError = NETWORK_NO_ERROR;
    //    if (readModeCounter == 7) 
    //    {
    //        network_process_byte(readModeError, readModeByte);
    //        readModeCounter = 0;
    //        readModeByte = 0;
            // Nach erfolgreichen lesen/schreiben TIMER2_OVF_vect deaktivieren
            // INT0_vect aktivieren
    //    } 
    //    else 
    //    {
    //        readModeByte |= NETWORK_SEND_PORT;
    //        readModeByte &= (0<<1);
    //        readModeCounter++;
    //    }
    //}

    //if (writeMode == true) 
    //{
    //    static uint8_t writeModeCounter = 0;
    //    static uint8_t writeModeError = NETWORK_NO_ERROR;
    //    if ((writeModeCounter < 7) && (writeModeError == 0)) 
    //    {
    //        if (writeModeByte & (1 << writeModeCounter))
    //        {
    //            writeModeError = network_send_one();
    //        }
    //        else
    //        {
    //            writeModeError = network_send_zero();
    //        }
    //        writeModeCounter++;
    //    }
    //    else
    //    {
    //        writeModeCounter = 0;
    //        writeModeError = 0;
    //        writeModeByte = 0;
    //        writeMode = false;
    //        network_pull_up();
    //        // Nach erfolgreichen lesen/schreiben TIMER2_OVF_vect deaktivieren
    //        // INT0_vect aktivieren
    //    }
    //}
}
