/* 
 *	Copyright (C) Chia-chen Kuo - April 2001
 *
 *  This file is part of DVD2AVI, a free MPEG-2 decoder
 *	
 *  DVD2AVI is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *   
 *  DVD2AVI is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *   
 *  You should have received a copy of the GNU General Public License
 *  along with GNU Make; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA. 
 *
 */

#include "global.h"

void CMPEG2Decoder::Initialize_Buffer()
{
	Rdptr = Rdbfr + BUFFER_SIZE;
	Rdmax = Rdptr;
	buffer_invalid = (unsigned char *) 0xffffffff;

	if (SystemStream_Flag)
	{
		if (Rdptr >= Rdmax)
			Next_Packet();
		CurrentBfr = *Rdptr++ << 24;

		if (Rdptr >= Rdmax)
			Next_Packet();
		CurrentBfr += *Rdptr++ << 16;

		if (Rdptr >= Rdmax)
			Next_Packet();
		CurrentBfr += *Rdptr++ << 8;

		if (Rdptr >= Rdmax)
			Next_Packet();
		CurrentBfr += *Rdptr++;

		Fill_Next();
	}
	else
	{
		Fill_Buffer();

		CurrentBfr = (*Rdptr << 24) + (*(Rdptr+1) << 16) + (*(Rdptr+2) << 8) + *(Rdptr+3);
		Rdptr += 4;

		Fill_Next();
	}

	BitsLeft = 32;
}

unsigned int CMPEG2Decoder::Get_Bits_All(unsigned int N)
{
	#ifdef PROFILING
//		start_bit_timer();
	#endif

	N -= BitsLeft;
	Val = (CurrentBfr << (32 - BitsLeft)) >> (32 - BitsLeft);

	if (N != 0)
		Val = (Val << N) + (NextBfr >> (32 - N));

	CurrentBfr = NextBfr;
	BitsLeft = 32 - N;
	Fill_Next();

	#ifdef PROFILING
//		stop_bit_timer();
	#endif

	return Val;
}

void CMPEG2Decoder::Flush_Buffer_All(unsigned int N)
{
	#ifdef PROFILING
//		start_bit_timer();
	#endif

	CurrentBfr = NextBfr;
	BitsLeft = BitsLeft + 32 - N;
	Fill_Next();

	#ifdef PROFILING
//		stop_bit_timer();
	#endif
}


typedef struct {			
	// 1 byte
	unsigned char sync_byte; // 		8	bslbf

	// 2 bytes
	unsigned char transport_error_indicator;//		1	bslbf
	unsigned char payload_unit_start_indicator;//		1	bslbf
	unsigned char transport_priority; //		1	bslbf
	unsigned short pid;	//	13	uimsbf

	// 1 byte
	unsigned char transport_scrambling_control;//		2	bslbf
	unsigned char adaptation_field_control;//		2	bslbf
	unsigned char continuity_counter;//		4	uimsbf

	// VVV (only valid if adaptation_field_control != 1)
	// 1 byte
		unsigned char adaptation_field_length; // 8 uimsbf 
	
		// VVV (only valid if adaptation_field_length != 0)
		// 1 byte
			unsigned char discontinuity_indicator; //	1	bslbf
			unsigned char random_access_indicator; //	1	bslbf
			unsigned char elementary_stream_priority_indicator; //	1	bslbf
			unsigned char PCR_flag; //	1	bslbf
			unsigned char OPCR_flag; //	1	bslbf
			unsigned char splicing_point_flag; //	1	bslbf
			unsigned char transport_private_data_flag; //	1	bslbf
			unsigned char adaptation_field_extension_flag; //	1	bslbf

	/*
	if(adaptation_field_control=='10'  || adaptation_field_control=='11'){			
		adaptation_field()			
	}			
	if(adaptation_field_control=='01' || adaptation_field_control=='11') {			
		for (i=0;i<N;i++){			
			data_byte		8	bslbf
		}			
	}			
	*/
	
} transport_packet;

#define	SKIP_TRANSPORT_PACKET_BYTES( bytes_to_skip ) \
{	Rdptr += (bytes_to_skip); Packet_Length -= (bytes_to_skip); }


void CMPEG2Decoder::Next_Transport_Packet()
{
	int Packet_Length;  // # bytes remaining in MPEG-2 transport packet
	unsigned int code;
	int tp_previous_continuity_counter = 0; // this isn't used 

	const transport_packet tp_zero ={0};  // 'ZERO'd' structure
	transport_packet tp ={0};  // temporary holding struct, zero it out

	// Packet_Length is like a 'position' index
	// 188 = start of packet (first byte), 0 = end of packet (last byte)

	for (;;)
	{
		// 0) initialize some temp variables
		Packet_Length = 188; // total length of 1 MPEG-2 transport packet
		tp_previous_continuity_counter = tp.continuity_counter; // update counter
		tp = tp_zero; // erase our holding structure

		// 1) search for sync-byte
		do {
		  tp.sync_byte = Get_Byte();
		}	while ( tp.sync_byte != 0x47 );
//		} while ( tp.sync_byte != 0x47 && tp.sync_byte != 0x72 &&
//							tp.sync_byte != 0x29);
		--Packet_Length; // decrement the sync_byte;

		// 2) get pid, transport_error_indicator, payload_unit_start_indicator
		code = Get_Short();
		Packet_Length = Packet_Length - 2; // decrement the two bytes we just got;
		tp.pid = code & 0x1FFF; // bits [12:0]
		tp.transport_error_indicator = (code >> 15) & 0x01;  // bit#15
		tp.payload_unit_start_indicator = (code >> 14) & 0x01; // bit#14
		tp.transport_priority = (code >> 13) & 0x01; // bit#13

		// 3) get other fields
		code = Get_Byte();
		--Packet_Length; // decrement the 1 byte we just got;
		tp.transport_scrambling_control = (code >> 6) & 0x03;//		2	bslbf
		tp.adaptation_field_control = (code >> 4 ) & 0x03;//		2	bslbf
		tp.continuity_counter = code & 0x0F;//		4	uimsbf


		// 4) check for early-exit conditions ... (possibly skip packet)
		// we don't care about the continuity counter
		// if ( tp.continuity_counter != previous_continuity_counter ) ...
		if ( tp.transport_error_indicator ||
				 (tp.adaptation_field_control == 0) )
		{
			// skip remaining bytes in current packet
			SKIP_TRANSPORT_PACKET_BYTES( Packet_Length )
			continue; // abort, and circle back to top of 'for() loop'
		}

		// 5) check 
		if ( tp.adaptation_field_control == 2 || tp.adaptation_field_control == 3)
		{
			// adaptation field is present
			tp.adaptation_field_length = Get_Byte(); // 8-bits
			--Packet_Length; // decrement the 1 byte we just got;

			if ( tp.adaptation_field_length != 0 ) // end of field already?
			{
				// if we made it this far, we no longer need to decrement
				// Packet_Length.  We took care of it up there!
				code = Get_Byte();
				--Packet_Length; // decrement the 1 byte we just got;
				tp.discontinuity_indicator = (code >> 7) & 0x01; //	1	bslbf
				tp.random_access_indicator = (code >> 6) & 0x01; //	1	bslbf
				tp.elementary_stream_priority_indicator = (code >> 5) & 0x01; //	1	bslbf
				tp.PCR_flag = (code >> 4) & 0x01; //	1	bslbf
				tp.OPCR_flag = (code >> 3) & 0x01; //	1	bslbf
				tp.splicing_point_flag = (code >> 2) & 0x01; //	1	bslbf
				tp.transport_private_data_flag = (code >> 1) & 0x01; //	1	bslbf
				tp.adaptation_field_extension_flag = (code >> 0) & 0x01; //	1	bslbf

				// skip the remainder of the adaptation_field
				SKIP_TRANSPORT_PACKET_BYTES( tp.adaptation_field_length-1 )
			} // if ( tp.adaptation_field_length != 0 )
		} // if ( tp.adaptation_field_control != 1 )

		// we've processed the header, so now just the payload is left...

		// video
//		if ( (tp.pid == 0x0021 || tp.pid == 0x0011 ) && (Packet_Length > 0) ) 
		if ( tp.pid == MPEG2_Transport_VideoPID && Packet_Length > 0 ) 
		{
			// we really should check for an MPEG-2 PES packet-header, but I'm lazy
			Rdmax = Rdptr + Packet_Length;
			return;
		}

		// fallthrouugh case
		// skip the remainder of the adaptation_field
		SKIP_TRANSPORT_PACKET_BYTES( Packet_Length )
	} // for
}

void CMPEG2Decoder::Next_Packet()
{
	unsigned int code, Packet_Length, Packet_Header_Length;

	if ( SystemStream_Flag == 2 )  // MPEG-2 transport packet?
	{
		Next_Transport_Packet();
		return;
	}

	for (;;)
	{
		code = Get_Short();
		code = (code<<16) + Get_Short();

		// remove system layer byte stuffing
		while ((code & 0xffffff00) != 0x00000100)
		{
			if (Fault_Flag == OUT_OF_BITS)
				return;
			code = (code<<8) + Get_Byte();
		}

		switch (code)
		{
			case PACK_START_CODE:
				Rdptr += 8;
				break;

			case VIDEO_ELEMENTARY_STREAM:   
				Packet_Length = Get_Short();
				Rdmax = Rdptr + Packet_Length;

				code = Get_Byte();

				if ((code & 0xc0)==0x80)
				{
					code = Get_Byte();
					Packet_Header_Length = Get_Byte();

					Rdptr += Packet_Header_Length;
					return;
				}
				else
					Rdptr += Packet_Length-1;
				break;

			default:
				if (code>=SYSTEM_START_CODE)
				{
					code = Get_Short();
					Rdptr += code;
				}
				break;
		}
	}
}

void CMPEG2Decoder::Fill_Buffer()
{
	#ifdef PROFILING
//		start_bit_timer();
	#endif

	Read = _read(Infile[File_Flag], Rdbfr, BUFFER_SIZE);

	if (Read < BUFFER_SIZE)
		Next_File();

	Rdptr = Rdbfr;

	if (SystemStream_Flag)
		Rdmax -= BUFFER_SIZE;

	#ifdef PROFILING
//		stop_bit_timer();
	#endif
}

void CMPEG2Decoder::Next_File()
{
	int bytes;

	if (File_Flag < File_Limit-1)
	{
		File_Flag ++;
	}
	else
	{
		Fault_Flag = OUT_OF_BITS;
		File_Flag = 0;
	}
	// Even if we ran out of files, we reread the first one, just so
	// the decoder at least processes valid data until it detects the
	// fault flag and exits.
	_lseeki64(Infile[File_Flag], 0, SEEK_SET);
	bytes = read(Infile[File_Flag], Rdbfr + Read, BUFFER_SIZE - Read);
	if (Read + bytes == BUFFER_SIZE)
		// The whole buffer has valid data.
		buffer_invalid = (unsigned char *) 0xffffffff;
	else
		// Point to the first invalid buffer location.
		buffer_invalid = Rdbfr + Read + bytes;
}

unsigned int CMPEG2Decoder::Show_Bits(unsigned int N)
{
	if (N <= BitsLeft) {
		return (CurrentBfr << (32 - BitsLeft)) >> (32 - N);;
	}
	else
	{
		N -= BitsLeft;
		return (((CurrentBfr << (32 - BitsLeft)) >> (32 - BitsLeft)) << N) + (NextBfr >> (32 - N));;
	}
}

unsigned int CMPEG2Decoder::Get_Bits(unsigned int N)
{
	if (N < BitsLeft)
	{
	#ifdef PROFILING
	//	start_bit_timer();
	#endif
		Val = (CurrentBfr << (32 - BitsLeft)) >> (32 - N);
		BitsLeft -= N;
	#ifdef PROFILING
	//	stop_bit_timer();
	#endif
		return Val;
	}
	else
		return Get_Bits_All(N);
}

void CMPEG2Decoder::Flush_Buffer(unsigned int N)
{
	#ifdef PROFILING
//		start_bit_timer();
	#endif

	if (N < BitsLeft)
		BitsLeft -= N;
	else
		Flush_Buffer_All(N);

	#ifdef PROFILING
//		stop_bit_timer();
	#endif
}

void CMPEG2Decoder::Fill_Next()
{
	#ifdef PROFILING
//		start_bit_timer();
	#endif

	if (Rdptr >= buffer_invalid)
	{
		Fault_Flag = OUT_OF_BITS;
		return;
	}

	if (SystemStream_Flag && Rdptr > Rdmax - 4)
	{
		if (Rdptr >= Rdmax)
			Next_Packet();
		NextBfr = Get_Byte() << 24;

		if (Rdptr >= Rdmax)
			Next_Packet();
		NextBfr += Get_Byte() << 16;

		if (Rdptr >= Rdmax)
			Next_Packet();
		NextBfr += Get_Byte() << 8;

		if (Rdptr >= Rdmax)
			Next_Packet();
		NextBfr += Get_Byte();
	}
	else if (Rdptr <= Rdbfr + BUFFER_SIZE - 4)
	{
		NextBfr = (*Rdptr << 24) + (*(Rdptr+1) << 16) + (*(Rdptr+2) << 8) + *(Rdptr+3);
		Rdptr += 4;
	}
	else
	{
		if (Rdptr >= Rdbfr+BUFFER_SIZE)
			Fill_Buffer();
		NextBfr = *Rdptr++ << 24;

		if (Rdptr >= Rdbfr+BUFFER_SIZE)
			Fill_Buffer();
		NextBfr += *Rdptr++ << 16;

		if (Rdptr >= Rdbfr+BUFFER_SIZE)
			Fill_Buffer();
		NextBfr += *Rdptr++ << 8;

		if (Rdptr >= Rdbfr+BUFFER_SIZE)
			Fill_Buffer();
		NextBfr += *Rdptr++;
	}

	#ifdef PROFILING
//		stop_bit_timer();
	#endif
}

unsigned int CMPEG2Decoder::Get_Byte()
{
	#ifdef PROFILING
//		start_bit_timer();
	#endif

	if (Rdptr >= buffer_invalid)
	{
		Fault_Flag = OUT_OF_BITS;
		return Rdptr[-1];
	}

	while (Rdptr >= (Rdbfr + BUFFER_SIZE))
	{
		Read = _read(Infile[File_Flag], Rdbfr, BUFFER_SIZE);

		if (Read < BUFFER_SIZE)
			Next_File();

		Rdptr -= BUFFER_SIZE;
		Rdmax -= BUFFER_SIZE;
	}

	#ifdef PROFILING
//		stop_bit_timer();
	#endif

	return *Rdptr++;
}

unsigned int CMPEG2Decoder::Get_Short()
{
	unsigned int i = Get_Byte();
	return (i<<8) + Get_Byte();
}

void CMPEG2Decoder::next_start_code()
{
	#ifdef PROFILING
//		start_bit_timer();
	#endif

	Flush_Buffer(BitsLeft & 7);

	while (Show_Bits(24) != 1)
	{
		if (Fault_Flag == OUT_OF_BITS)
			return;
		Flush_Buffer(8);
	}

	#ifdef PROFILING
//		stop_bit_timer();
	#endif
}