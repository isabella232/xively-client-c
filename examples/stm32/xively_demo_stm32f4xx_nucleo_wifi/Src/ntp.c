/*TODO: This interface is currently blocking at sntp_get_datetime() and only
 *      supports 1 concurrent request. Change last_sntp_response to use a
 *      flexible array and we can make it non-blocking and support as many
 *      concurrent requests as necessary
 */
#include "stdio.h"
#include "string.h"
#include "wifi_interface.h"
#include "wifi_module.h"
#include "wifi_globals.h"

#include "ntp.h"

int32_t sntp_current_time = 0;
sntp_response_t* last_sntp_response = NULL;

/**
   * Packet description:
   *  - Flags 1 byte
   *      * Leap: 3 bits
   *      * Version: 3 bits
   *      * Mode: 2 bits
   *  - Stratum 1 byte
   *  - Polling 1 byte
   *  - Precision 1 byte
   *  - Root Delay 4 bytes
   *  - Root Dispersion 4 bytes
   *  - Reference Identifier 4 bytes
   *  - Reference Timestamp 8 bytes
   *  - Origin Timestamp 8 bytes
   *  - Receive Timestamp 8 bytes
   *  - Transmit Timestamp 8 bytes
   */
static const char SNTP_REQUEST[SNTP_MSG_SIZE] = {
0xe3, 0x00, 0x03, 0xfa, 0x00, 0x01, 0x00, 0x00,
0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0xd5, 0x22, 0x0e, 0x35, 0xb8, 0x76, 0xab, 0xea};

static WiFi_Status_t    sntp_start( char* sntp_server, uint32_t sntp_port, uint8_t* sock_id );
static WiFi_Status_t    sntp_send_request( uint8_t sock_id );
static sntp_status_t    sntp_await_response( uint8_t sock_id );
static sntp_response_t* sntp_malloc_response( void );
static int32_t          sntp_parse_response( char* response );
static void             sntp_free_response( sntp_response_t** r );
static WiFi_Status_t    sntp_stop( uint8_t sock_id );

static uint32_t sntp_ntohl( uint32_t n );

/**
   * @brief  initialize a new sntp_response_t. malloc() the struct and the
   *         48 byte char* response inside it.
   * @param  None
   * @retval WiFi_Status_t: WiFi_MODULE_SUCCESS on success, see wifi_interface.h
   */
static sntp_response_t* sntp_malloc_response( void )
{
    sntp_response_t* new_response = NULL;
    new_response = malloc(sizeof(sntp_response_t));
    if( NULL == new_response )
    {
        goto malloc_error;
    }
    new_response->socket_id = -1; //Default to invalid socket ID
    new_response->response = malloc(SNTP_MSG_SIZE);
    if( NULL == new_response->response )
    {
        goto malloc_error;
    }
    memset(new_response->response, 0, sizeof(sntp_response_t));
    return new_response;

malloc_error:
    sntp_free_response(&new_response);
    return NULL;
}

/**
   * @brief  free the space used by an sntp_response_t and set it to NULL
   * @param  **sntp_response_t we want to free()
   * @retval None
   */
static void sntp_free_response( sntp_response_t** r )
{
    if( NULL == *r )
    {
        return;
    }
    if( NULL != (*r)->response )
    {
        free((*r)->response);
    }
    free(*r);
    *r = NULL;
}

/**
   * @brief  Create a UDP socket to the SNTP server and return its ID by
   *         setting *sock_id
   * @param  *sntp_server is a char array with the relevant URL
   *         sntp_port is the port to be used for SNTP communication
   *         *sock_id will be set to the appropriate socket ID
   * @retval WiFi_Status_t: WiFi_MODULE_SUCCESS on success, see wifi_interface.h
   */
static WiFi_Status_t sntp_start( char* sntp_server, uint32_t sntp_port, uint8_t* sock_id )
{
    WiFi_Status_t status = WiFi_MODULE_SUCCESS;
    uint8_t sntp_protocol = 'u'; //UDP
    printf("\r\n\tOpening UDP socket to SNTP server %s:%lu",
           sntp_server,
           sntp_port);
    status = wifi_socket_client_open((uint8_t*)sntp_server, sntp_port,
                                     &sntp_protocol, sock_id);

    return status;
}

/**
   * @brief
   * @param  sock_id is the socket ID set by sntp_start() in stnp_start()
   * @retval WiFi_Status_t: WiFi_MODULE_SUCCESS on success, see wifi_interface.h
   */
static WiFi_Status_t sntp_stop( uint8_t sock_id )
{
    WiFi_Status_t status = WiFi_MODULE_SUCCESS;
    printf("\r\n>>Closing UDP socket to SNTP server...");
    status = wifi_socket_client_close(sock_id);
    if(status != WiFi_MODULE_SUCCESS)
    {
        printf("\r\n\tUDP Socket Close [FAIL] Status code: %d", status);
        return status;
    }
    else
    {
        printf("\r\n\tUDP Socket Close [OK]");
    }
    return status;
}

/**
   * @brief  Send SNTP packet to the Server. Must be called after a socket has
   *         has been created from sntp_get_datetime()->sntp_start()
   * @param  sock_id is the socket ID set by sntp_start()
   * @retval WiFi_Status_t: WiFi_MODULE_SUCCESS on success, see wifi_interface.h
   */
static WiFi_Status_t sntp_send_request( uint8_t sock_id )
{
    WiFi_Status_t status = WiFi_MODULE_SUCCESS;

    printf("\r\n>>Sending SNTP request to the server");
    status = wifi_socket_client_write(sock_id, SNTP_MSG_SIZE, (char*)SNTP_REQUEST);

    if(status != WiFi_MODULE_SUCCESS)
    {
        printf("\r\n\tSNTP Message Write [FAIL] Status code: %d", status);
    }
    else
    {
        printf("\r\n\tSNTP Message Write [OK]");
    }
    return status;
}

/**
   * @brief  Convert uint32_t from Network byte order to Host byte order.
   *         i.e. Return the reverse-endian value of the received argument.
   *         Implementation lifted from the Cube SDK's LwIP
   * @param  n is a 32bit integer in network order
   * @retval Returns a 32bit integer in host order (ready to be used by the uC)
   */
static uint32_t sntp_ntohl(uint32_t n)
{
    return ((n & 0xff) << 24)      |
           ((n & 0xff00) << 8)     |
           ((n & 0xff0000UL) >> 8) |
           ((n & 0xff000000UL) >> 24);
}

/**
   * @brief  Parses SNTP response and returns current date and time
   * @param  48 byte array with the SNTP response. Length MUST be validated
   *         before calling this function
   * @retval Current date and time in epoch format, -1 on error
   */
static int32_t sntp_parse_response( char* response )
{
    int32_t current_ntp_time = 0; //epoch + 2208988800
    int32_t current_epoch_time = 0;

    printf("\r\n>>Parsing SNTP response...");
    memcpy(&current_ntp_time, response+40, 4); //timestamp starts at byte 40
    current_ntp_time = sntp_ntohl(current_ntp_time); //Convert endianness
    current_epoch_time = current_ntp_time - 2208988800; //Remove NTP offset
    return current_epoch_time;
}

/**
   * @brief  Create new UDP socket to SNTP server, send SNTP request, await
   *         response for up to SNTP_TIMEOUT_MS, close socket and return
   * @param  - sock_id is a pre-allocated uint8_t pointer used to filter the WiFi
   *         API callbacks. It will be set to the socket ID returned by open()
   *         - epoch_time will be set to the current epoch time as returned by the
   *         SNTP server
   * @retval sntp_status_t: 0 means SNTP_SUCCESS, <0 means something failed.
   *         See ntp.h to handle different failure reasons in different ways
   */
sntp_status_t sntp_get_datetime( uint8_t* sock_id, int32_t* epoch_time )
{
    sntp_status_t retval = SNTP_SUCCESS; //Returned by this function
    WiFi_Status_t wifi_retval = WiFi_MODULE_SUCCESS;

    /* Create socket */
    printf("\r\n>>Getting date and time from SNTP server...");
    wifi_retval = sntp_start(SNTP_SERVER, SNTP_PORT, sock_id);
    if( wifi_retval != WiFi_MODULE_SUCCESS )
    {
        printf("\r\n\tSNTP socket creation [FAIL] Retval: %d", wifi_retval);
        retval = SNTP_SOCKET_ERROR;
        goto terminate;
    }
    else if(*sock_id < 0)
    {
        printf("\r\n\tUDP socket creation [FAIL] Socket ID<0: %d", *sock_id);
        retval = SNTP_SOCKET_ERROR;
        goto terminate;
    }
    printf("\r\n\tUDP socket creation [OK] Assigned socket ID: %d", *sock_id);

    /* Send SNTP request */
    wifi_retval = sntp_send_request(*sock_id);
    if( wifi_retval != WiFi_MODULE_SUCCESS )
    {
        printf("\r\n\tSNTP send_request [FAIL] Retval: %d", wifi_retval);
        retval = SNTP_REQUEST_FAILURE;
        goto terminate;
    }

    /* Await SNTP Response */
    printf("\r\n\tAwaiting server response");
    if( sntp_await_response(*sock_id) < 0 )
    {
        printf("\r\n\tSNTP response [FAIL] Response timed out");
        retval = SNTP_TIMEOUT;
        goto terminate;
    }

    if( *sock_id != last_sntp_response->socket_id )
    {
        printf("\r\n\tSocket ID assertion [FAIL] Bug in ntp.c?");
        retval = SNTP_INTERNAL_ERROR;
        goto terminate;
    }

    /* Parse server response */
    printf("\r\n\tParsing and converting SNTP response");
    sntp_current_time = sntp_parse_response(last_sntp_response->response);
    if( sntp_current_time < 0 )
    {
        printf("\r\n\tSNTP Parsing and conversion [FAIL]");
        retval = SNTP_PARSER_ERROR;
    }
    else
    {
        printf("\r\n\tReceived epoch time: %ld", sntp_current_time);
        retval = SNTP_SUCCESS;
    }

terminate:
    sntp_free_response(&last_sntp_response);

    /* Close socket */
    if( *sock_id > 0 )
    {
        sntp_stop(*sock_id);
    }
    return retval;
}

/**
   * @brief
   * @param  sock_id we're interested in
   * @retval SNTP_SUCCESS (0) or SNTP_TIMEOUT(-1)
   */
static sntp_status_t sntp_await_response( uint8_t sock_id )
{
    int32_t sntp_timeout_ms = SNTP_TIMEOUT_MS;
    const int32_t timeout_step = 250;
    while( NULL == last_sntp_response )
    {
        if( sntp_timeout_ms <= 0 )
        {
            return SNTP_TIMEOUT;
        }
        printf(".");
        HAL_Delay(timeout_step);
        sntp_timeout_ms -= timeout_step;
    }
    return SNTP_SUCCESS;
}

/**
   * @brief  This function shall be called from ind_wifi_socket_data_received(),
   *         when the sock_id is the one we got from sntp_get_datetime()
   * @param  See description for ind_wifi_socket_data_received
   * @retval None
   */
void sntp_socket_data_callback(uint8_t sock_id, uint8_t* data_ptr,
                               uint32_t message_size, uint32_t chunk_size)
{
    /* Verify we got an SNTP response, not just protocol data */
    if( (SNTP_MSG_SIZE != message_size) || (SNTP_MSG_SIZE != chunk_size) )
    {
        printf("\r\n\tMessage isn't an SNTP respone. It will be ignored");
        return;
    }

    /* Store SNTP response */
    sntp_free_response(&last_sntp_response);
    last_sntp_response = sntp_malloc_response();
    if( NULL == last_sntp_response )
    {
        printf("\r\n\tMemory allocation for SNTP response [FAIL] Msg ignored");
        return;
    }
    memcpy(last_sntp_response->response, (char*)data_ptr, SNTP_MSG_SIZE);
    last_sntp_response->socket_id = sock_id;

}
