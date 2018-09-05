///////////////////////////////////////////////////////////////////////////////
/**
 * @file        SerialModbusMaster.cpp
 *
 * @author      legicore
 *
 * @brief       xxx
 */
///////////////////////////////////////////////////////////////////////////////

#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <Arduino.h>

#include "SerialModbusConfig.h"
#include "SerialModbusBase.h"
#include "SerialModbusMaster.h"

/*-----------------------------------------------------------*/

SerialModbusMaster::SerialModbusMaster()
{
    vSetState( MASTER_IDLE );

    xReplyDataSize = 0;
    
    pxRequestData = NULL;
    
    ulTurnaroundDelayUs = configTURNAROUND_DELAY_US;
    ulResponseTimeoutUs = configRESPONSE_TIMEOUT_US;
    
    ulTimerResponseTimeoutUs = 0;
    ulTimerTurnaroundDelayUs = 0;
}
/*-----------------------------------------------------------*/

void SerialModbusMaster::vSetState( MBMasterState_t xStatePar )
{
    xState = xStatePar;
}
/*-----------------------------------------------------------*/

void SerialModbusMaster::vStartTurnaroundDelay( void )
{
    ulTimerTurnaroundDelayUs = micros();
}
/*-----------------------------------------------------------*/

void SerialModbusMaster::vStartResponseTimeout( void )
{
    ulTimerResponseTimeoutUs = micros();
}
/*-----------------------------------------------------------*/

boolean SerialModbusMaster::bTimeoutTurnaroundDelay( void )
{
    return ( micros() - ulTimerTurnaroundDelayUs ) > ulTurnaroundDelayUs;
}
/*-----------------------------------------------------------*/

boolean SerialModbusMaster::bTimeoutResponseTimeout( void )
{
    return ( micros() - ulTimerResponseTimeoutUs ) > ulResponseTimeoutUs;
}
/*-----------------------------------------------------------*/

void SerialModbusMaster::setResponseTimeout( uint32_t timeMs )
{
    ulResponseTimeoutUs = timeMs * 1000;
}
/*-----------------------------------------------------------*/

void SerialModbusMaster::setTurnaroundDelay( uint32_t timeMs )
{
    ulTurnaroundDelayUs = timeMs * 1000;
}
/*-----------------------------------------------------------*/

MBStatus_t SerialModbusMaster::xProcessDataList( void )
{
    if( pxDataList != NULL )
    {
        if( pxDataList[ xDataListIndex ].functionCode == 0x00 )
        {
            xDataListIndex = 0;
        }
        
        return setRequest( &pxDataList[ xDataListIndex++ ] );
    }
    else
    {
        xDataListIndex = 0;
    }
    
    return OK;
}
/*-----------------------------------------------------------*/

MBStatus_t SerialModbusMaster::setRequest( MBData_t * request )
{
    xSetException( OK );
    
    if( request == NULL )
    {
        return xSetException( NOK_NULL_POINTER );
    }
    
    if( request->id            > configID_SLAVE_MAX ||
        request->functionCode == 0x00               ||
        request->address      == 0x0000             ||
        request->objectSize    < 1                  )
    {
        return xSetException( NOK_LIST_ENTRY );
    }
	
    vClearRequestFrame();
    
    pucRequestFrame[ 0 ] = request->id;
    pucRequestFrame[ 1 ] = request->functionCode;
    pucRequestFrame[ 2 ] = highByte( request->address );
    pucRequestFrame[ 3 ] =  lowByte( request->address );
    
    switch( request->functionCode )
    {
#if( ( configFC03 == 1 ) || ( configFC04 == 1 ) )
        case READ_HOLDING_REGISTERS:
        case READ_INPUT_REGISTERS:
        {
            pucRequestFrame[ 4 ] = highByte( request->objectSize );
            pucRequestFrame[ 5 ] =  lowByte( request->objectSize );
            xRequestLength = 6;
            
            break;
        }
#endif
#if( ( configFC05 == 1 ) || ( configFC06 == 1 ) )
        case WRITE_SINGLE_COIL:
        case WRITE_SINGLE_REGISTER:
        {
            pucRequestFrame[ 4 ] = highByte( request->object[ 0 ] );
            pucRequestFrame[ 5 ] =  lowByte( request->object[ 0 ] );
            xRequestLength = 6;
            
            break;
        }
#endif
#if( configFC16 == 1 )
        case WRITE_MULTIPLE_REGISTERS:
        {
            /* Quantity */
            pucRequestFrame[ 4 ] = highByte( request->objectSize );
            pucRequestFrame[ 5 ] =  lowByte( request->objectSize );
            /* Byte-Count */
            pucRequestFrame[ 6 ] = ( uint8_t ) request->objectSize * 2;
            
            xRequestLength = 7;
            
            if( request->object != NULL )
            {
                for( size_t i = 0; i < request->objectSize; i++ )
                {
                    pucRequestFrame[ ( i * 2 ) + 7 ] = highByte( request->object[ i ] );
                    pucRequestFrame[ ( i * 2 ) + 8 ] =  lowByte( request->object[ i ] );
                    xRequestLength += 2;
                }
            }
            else
            {
                return xSetException( NOK_LIST_ENTRY );
            }
            
            break;
        }
#endif
        default:
        {
			return xSetException( ILLEGAL_FUNCTION );
        }
    }
    
    pxRequestData = request;

    return xSetChecksum( pucRequestFrame, &xRequestLength );
}
/*-----------------------------------------------------------*/

MBStatus_t SerialModbusMaster::processModbus( void )
{
    if( xProcessDataList() != OK )
    {
        vSetState( PROCESSING_ERROR );
    }
    
    do
    {
        /* Get the current state and select the associated action. */
        switch( xState )
        {
            case MASTER_IDLE :
            {
                vClearReplyFrame();

                #if( configMODE == configMODE_ASCII )
                {
                    /* Convert request to ascii and update the pdu length */
                    xRtuToAscii( pucRequestFrame, &mbRequestLength );
                }
                #endif

                vSendData( pucRequestFrame, xRequestLength );

                /* Check for broadcast or normal reqest */
#if( configMODE == configMODE_RTU )
                if( ucREQUEST_ID == configID_BROADCAST )
#endif
#if( configMODE == configMODE_ASCII )
                if( ucAsciiToByte( pucRequestFrame[ 1 ], pucRequestFrame[ 2 ] ) == configID_BROADCAST )
#endif
                {
                    /* Broadcast (request id is 0) */
                    vSetState( WAITING_TURNAROUND_DELAY );

                    /* Start timer for turnaround-delay */
                    vStartTurnaroundDelay();
                }
                else
                {
                    /* Normal request */
                    vSetState( WAITING_FOR_REPLY );

                    /* Start timer for resposne-timeout */
                    vStartResponseTimeout();
                }

                break;
            }

            case WAITING_TURNAROUND_DELAY :
            {
                if( bTimeoutTurnaroundDelay() == true )
                {
                    /* Nothing ... clear buffer and go on */
                    vClearRequestFrame();
                    vSetState( MASTER_IDLE );
                }

                break;
            }

            case WAITING_FOR_REPLY :
            {
                if( xReplyLength < configMAX_FRAME_SIZE )
                {
                    if( bReceiveByte( pucReplyFrame, &xReplyLength ) == true )
                    {
                        #if( configMODE == configMODE_RTU )
                        {
                            if( ucREPLY_ID == ucREQUEST_ID )
                            {
                                vStartInterFrameDelay();
                                break;
                            }
                        }
                        #endif

                        #if( configMODE == configMODE_ASCII )
                        {
                            if( pucReplyFrame[ 0 ] == ':' )
                            {
                                break;
                            }
                        }
                        #endif

                        vClearReplyFrame();
                        break;
                    }
                }
                else
                {
                    vSetState( PROCESSING_ERROR );
                    xSetException( NOK_RX_OVERFLOW );
                    break;
                }
                
                if( bTimeoutResponseTimeout() == true )
                {
                    vSetState( PROCESSING_ERROR );
                    xSetException( NOK_NO_REPLY );
                    break;
                }
                
                if( xReplyLength > 0 )
                {
                    #if( configMODE == configMODE_RTU )
                    {
                        if( bTimeoutInterFrameDelay() == true )
                        {
                            vSetState( PROCESSING_REPLY );
                        }
                    }
                    #endif

                    #if( configMODE == configMODE_ASCII )
                    {
                        /* Check for Newline (frame end) */
                        if( pucReplyFrame[ mbReplyLength - 1 ] == cAsciiInputDelimiter )
                        {
                            /* Check for Carriage Return (frame end) */
                            if( pucReplyFrame[ mbReplyLength - 2 ] == '\r' )
                            {
                                /* Convert the frame from rtu to ascii format */
                                xAsciiToRtu( pucReplyFrame, &mbReplyLength );
                                xAsciiToRtu( pucRequestFrame, &mbRequestLength );

                                if( ucREPLY_ID == configREQUEST_ID )
                                {
                                    vSetState( PROCESSING_REPLY );
                                }
                            }
                        }
                    }
                    #endif
                }

                break;
            }

            case PROCESSING_REPLY :
            {
                if( xCheckChecksum( pucReplyFrame, &xReplyLength ) == OK )
                {
                    switch( ucREPLY_FUNCTION_CODE )
                    {
#if( ( configFC03 == 1 ) || ( configFC04 == 1 ) )
                        case READ_HOLDING_REGISTERS :
                        case READ_INPUT_REGISTERS :
                        {
                            vHandler03_04();
                            break;
                        }
#endif
#if( configFC05 == 1 )
                        case WRITE_SINGLE_COIL :
                        {
                            vHandler05();
                            break;
                        }
#endif
#if( configFC06 == 1 )
                        case WRITE_SINGLE_REGISTER :
                        {
                            vHandler06();
                            break;
                        }
#endif
#if( configFC16 == 1 )
                        case WRITE_MULTIPLE_REGISTERS :
                        {
                            vHandler16();
                            break;
                        }
#endif
                        default :
                        {
                            /* Check for Error Code */
                            if( ucREPLY_FUNCTION_CODE == ( ucREQUEST_FUNCTION_CODE | 0x80 ) )
                            {
                                xSetException( ( MBException_t ) ucREPLY_ERROR_CODE );
                                vSetState( PROCESSING_ERROR );
                            }
                        }
                    }

                    if( xState != PROCESSING_ERROR )
                    {
                        vSetState( MASTER_IDLE );
                    }
                }
                else
                {
                    vSetState( MASTER_IDLE );
                }

                break;
            }

            case PROCESSING_ERROR :
            {
                vClearRequestFrame();
                vClearReplyFrame();
                xReplyDataSize = 0;

                vSetState( MASTER_IDLE );

                break;
            }

            default :
            {
                xSetException( NOK_PROCESS_STATE );
                vSetState( MASTER_IDLE );
            }
        }
        
        #if( configPROCESS_LOOP_HOOK == 1 )
        {
            if( ( vProcessLoopHook != NULL ) && ( xState != MASTER_IDLE ) )
            {
                (*vProcessLoopHook)();
            }
        }
        #endif
    }
    while( xState != MASTER_IDLE );
    
    return xException;
}
/*-----------------------------------------------------------*/

size_t SerialModbusMaster::getReplyDataSize( void )
{
    return xReplyDataSize;
}
/*-----------------------------------------------------------*/

size_t SerialModbusMaster::getReplyData( uint16_t * buffer, size_t bufferSize )
{
    if( ( buffer == NULL ) || ( bufferSize <= 0 ) || ( bufferSize > xReplyDataSize ) )
    {
        return 0;
    }
    
    switch( ucREPLY_FUNCTION_CODE )
    {
#if( configFC03 == 1 || configFC04 == 1 )
        case READ_HOLDING_REGISTERS:
        case READ_INPUT_REGISTERS:
        {
            for( size_t i = 0; i < bufferSize; i++ )
            {
                buffer[ i ] = usReplyWord( i );
            }
            return bufferSize;
        }
#endif
#if( configFC05 == 1 )
        case WRITE_SINGLE_COIL:
        {
            buffer[ 0 ] = usREQUEST_COIL_VALUE;
            return bufferSize;
        }
#endif
#if( configFC06 == 1 )
        case WRITE_SINGLE_REGISTER:
        {
            buffer[ 0 ] = usREPLY_REGISTER_VALUE;
            return bufferSize;
        }
#endif
        default:
        {
            return 0;
        }
    }

    return 0;
}
/*-----------------------------------------------------------*/

#if( ( configFC03 == 1 ) || ( configFC04 == 1 ) )
void SerialModbusMaster::vHandler03_04( void )
{
    size_t xOffset = 0;

    /* Check the response byte count */
    if( ucREPLY_BYTE_COUNT == ( uint8_t ) ( 2 * usREQUEST_QUANTITY ) )
    {
        if( pxRequestData->object != NULL )
        {
            xOffset = ( size_t ) ( usREQUEST_ADDRESS - pxRequestData->address );
            
            for( size_t i = 0; i < ( size_t ) usREQUEST_QUANTITY; i++ )
            {
                pxRequestData->object[ i + xOffset ] = usReplyWord( i );
            }
        }
        
        xReplyDataSize = ( size_t ) usREQUEST_QUANTITY;

        if( pxRequestData->action != NULL )
        {
            (*pxRequestData->action)();
        }
    }
    else
    {
        vSetState( PROCESSING_ERROR );
        xSetException( NOK_BYTE_COUNT );
    }
}
#endif
/*-----------------------------------------------------------*/

#if( configFC05 == 1 )
void SerialModbusMaster::vHandler05( void )
{
    /* Check the response output address */
    if( usREPLY_ADDRESS == usREQUEST_ADDRESS )
    {
        /* Check the response output value */
        if( usREPLY_COIL_VALUE == usREQUEST_COIL_VALUE )
        {
            xReplyDataSize = 1;

            if( pxRequestData->action != NULL )
            {
                (*pxRequestData->action)();
            }
        }
        else
        {
            vSetState( PROCESSING_ERROR );
            xSetException( NOK_COIL_VALUE );
        }
    }
    else
    {
        vSetState( PROCESSING_ERROR );
        xSetException( NOK_OUTPUT_ADDRESS );
    }
}
#endif
/*-----------------------------------------------------------*/

#if( configFC06 == 1 )
void SerialModbusMaster::vHandler06( void )
{
    /* Check the response output address */
    if( usREPLY_ADDRESS == usREQUEST_ADDRESS )
    {
        /* Check the response output value */
        if( usREPLY_OUTPUT_VALUE == usREQUEST_OUTPUT_VALUE )
        {
            xReplyDataSize = 1;

            if( pxRequestData->action != NULL )
            {
                (*pxRequestData->action)();
            }
        }
        else
        {
            vSetState( PROCESSING_ERROR );
            xSetException( NOK_OUTPUT_VALUE );
        }
    }
    else
    {
        vSetState( PROCESSING_ERROR );
        xSetException( NOK_OUTPUT_ADDRESS );
    }
}
#endif
/*-----------------------------------------------------------*/

#if( configFC16 == 1 )
void SerialModbusMaster::vHandler16( void )
{
    /* Check the response output address */
    if( usREPLY_ADDRESS == usREQUEST_ADDRESS )
    {
        /* Check the response output value */
        if( usREPLY_QUANTITY == usREQUEST_QUANTITY )
        {
            xReplyDataSize = 0;

            if( pxRequestData->action != NULL )
            {
                (*pxRequestData->action)();
            }
        }
        else
        {
            vSetState( PROCESSING_ERROR );
            xSetException( NOK_QUANTITY );
        }
    }
    else
    {
        vSetState( PROCESSING_ERROR );
        xSetException( NOK_OUTPUT_ADDRESS );
    }
}
#endif