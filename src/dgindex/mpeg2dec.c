/* 
 *  Mutated into DGIndex. Modifications Copyright (C) 2004, Donald Graft
 * 
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
#include "getbit.h"

static BOOL GOPBack(void);
static void InitialDecoder(void);

DWORD WINAPI MPEG2Dec(LPVOID n)
{
	int i;
	extern int closed_gop, VideoPTS;
	extern FILE *mpafp;
	__int64 saveloc;

	Pause_Flag = Stop_Flag = Start_Flag = Fault_Flag = false;
	Frame_Number = Second_Field = 0;
	VOB_ID = CELL_ID = 0;
	Sound_Max = 1; Bitrate_Monitor = 0;

	for (i=0; i<CHANNEL; i++)
	{
		ZeroMemory(&ac3[i], sizeof(AC3Stream));
		ZeroMemory(&mpa[i], sizeof(RAWStream));	
		ZeroMemory(&dts[i], sizeof(RAWStream));
	}

	ZeroMemory(&pcm, sizeof(struct PCMStream));
	ZeroMemory(&Channel, sizeof(Channel));
	mpafp = NULL;

	switch (process.locate)
	{
		case LOCATE_FORWARD:
			process.startfile = process.file;
			process.startloc = (process.lba + 1) * BUFFER_SIZE;

			process.end = process.total - BUFFER_SIZE;
			process.endfile = File_Limit - 1;
			process.endloc = (process.length[File_Limit-1]/BUFFER_SIZE - 1) * BUFFER_SIZE;
			break;

		case LOCATE_BACKWARD:
			process.startfile = process.file;
			process.startloc = process.lba * BUFFER_SIZE;
			process.end = process.total - BUFFER_SIZE;
			process.endfile = File_Limit - 1;
			process.endloc = (process.length[File_Limit-1]/BUFFER_SIZE - 1) * BUFFER_SIZE;
			if (GOPBack() == false && File_Flag > 0)
			{
				// Trying to step back to previous VOB file.
				process.startfile--;
				File_Flag--;
				process.startloc = process.length[process.startfile];
				process.run = 0;
				for (i=0; i<process.startfile; i++)
					process.run += process.length[i];
				process.start = process.run + process.startloc;
				GOPBack();
			}
			break;

		case LOCATE_RIP:
			process.startfile = process.leftfile;
			process.startloc = process.leftlba * BUFFER_SIZE;
			process.endfile = process.rightfile;
			process.endloc = (process.rightlba - 1) * BUFFER_SIZE;
			goto do_rip_play;
		case LOCATE_PLAY:
			process.startfile = process.file;
			process.startloc = process.lba * BUFFER_SIZE;
			process.endfile = File_Limit - 1;
			process.locate = LOCATE_RIP;
do_rip_play:
			process.run = 0;
			for (i=0; i<process.startfile; i++)
				process.run += process.length[i];
			process.start = process.run + process.startloc;

			process.end = 0;
			for (i=0; i<process.endfile; i++)
				process.end += process.length[i];
			process.end += process.endloc;
			break;

		case LOCATE_SCROLL:
			File_Flag = process.startfile;
			_lseeki64(Infile[process.startfile], (process.startloc/BUFFER_SIZE)*BUFFER_SIZE, SEEK_SET);
			Initialize_Buffer();

			process.op = 0;
			saveloc = process.startloc;

			while (true)
			{
				if (Get_Hdr() == 0)
				{
					GOPBack();
					break;
				}
				else if (picture_coding_type == I_TYPE)
				{
					process.startloc = saveloc;
					break;
				}
			}
			break;
	}

	// search MPEG-2 Sequence Header
	if (!Check_Flag)
	{
		int code;

		File_Flag = 0;
		_lseeki64(Infile[0], 0, SEEK_SET);
		Initialize_Buffer();

		while (!Check_Flag)
		{
			next_start_code();
			code = Get_Bits(32);

			switch (code)
			{
				case PACK_START_CODE:
					SystemStream_Flag = 1;
					break;

				case SEQUENCE_HEADER_CODE:
					// Can't currently decode MPEG1, so detect that.
					next_start_code();
					code = Get_Bits(32);
					if (code == EXTENSION_START_CODE && Get_Bits(4) == 1)
					{
						// The SEQUENCE_HEADER_CODE is real.
						File_Flag = 0;
						_lseeki64(Infile[0], 0, SEEK_SET);
						Initialize_Buffer();
						while (true)
						{
							next_start_code();
							code = Get_Bits(32);
							if (code == SEQUENCE_HEADER_CODE) break;
						}
						sequence_header();
						InitialDecoder();
						Check_Flag = true;
					}
					else
					{
						MessageBox(hWnd, "MPEG1 streams not currently supported.",
								   NULL, MB_OK | MB_ICONERROR | MB_SYSTEMMODAL);
						return 0;
					}
			}
		}

		// Determine the number of leading B frames in display order.
		// This will be used to warn the user if required and to
		// adjust the VideoPTS.
		saveloc = process.startloc;
		File_Flag = 0;
		_lseeki64(Infile[0], 0, SEEK_SET);
		Initialize_Buffer();
		while (Get_Hdr() && picture_coding_type != I_TYPE)
		{
			volatile int i;
			i = 5;
		}
		LeadingBFrames = 0;
		while (Get_Hdr() && picture_coding_type == B_TYPE)
		{
			LeadingBFrames++;
			if (gop_warned == false && !closed_gop)
			{
				MessageBox(hWnd, "WARNING! Opening GOP is not closed.\nThe first few frames may not be decoded correctly.", NULL, MB_OK | MB_ICONERROR);
				gop_warned = true;
			}
		}
		// Position back to the first GOP.
		process.startloc = saveloc;
	}

	Frame_Rate = (FO_Flag==FO_FILM) ? frame_rate * 0.8f : frame_rate;

	if (D2V_Flag)
	{
		i = File_Limit;

		// The first parameter is the format version number.
		// It must be coordinated with the test in MPEG2DEC3dg
		// and used to reject obsolete D2V files.
		fprintf(D2VFile, "DGIndexProjectFile%s\n%d\n", "06", i);
		while (i)
		{
			fprintf(D2VFile, "%d %s\n", strlen(Infilename[File_Limit-i]), Infilename[File_Limit-i]);
			i--;
		}

		fprintf(D2VFile, "\nStream_Type=%d\n", SystemStream_Flag);

		if (SystemStream_Flag == 2)
			fprintf(D2VFile, "MPEG2_Transport_PID=%x,%x\n", MPEG2_Transport_VideoPID, MPEG2_Transport_AudioPID);

		fprintf(D2VFile, "iDCT_Algorithm=%d (1:MMX 2:SSEMMX 3:FPU 4:REF 5:SSE2MMX)\n", iDCT_Flag);
		fprintf(D2VFile, "YUVRGB_Scale=%d (0:TVScale 1:PCScale)\n", Scale_Flag);
		if (Luminance_Flag)
		{
			fprintf(D2VFile, "Luminance_Filter=%d,%d (Gamma, Offset)\n", LumGamma, LumOffset);
		}
		else
		{
			fprintf(D2VFile, "Luminance_Filter=0,0 (Gamma, Offset)\n");
		}

		if (ClipResize_Flag)
		{
			fprintf(D2VFile, "Clipping=%d,%d,%d,%d (ClipLeft, ClipRight, ClipTop, ClipBottom)\n", 
				Clip_Left, Clip_Right, Clip_Top, Clip_Bottom);
		}
		else
		{
			fprintf(D2VFile, "Clipping=0,0,0,0 (ClipLeft, ClipRight, ClipTop, ClipBottom)\n");
		}

		fprintf(D2VFile, "Aspect_Ratio=%s\n", AspectRatio[aspect_ratio_information]);
		fprintf(D2VFile, "Picture_Size=%dx%d\n", Coded_Picture_Width, Coded_Picture_Height);
		fprintf(D2VFile, "Field_Operation=%d (0:None 1:ForcedFILM 2:RawFrames)\n", FO_Flag);
		fprintf(D2VFile, "Frame_Rate=%d\n", (int)(Frame_Rate*1000));
		fprintf(D2VFile, "Location=%d,%X,%d,%X\n\n", process.leftfile, (int)process.leftlba, 
			process.rightfile, (int)process.rightlba);
	}

	File_Flag = process.startfile;
	_lseeki64(Infile[process.startfile], (process.startloc/BUFFER_SIZE)*BUFFER_SIZE, SEEK_SET);
	Initialize_Buffer();

	process.op = 0;

	while (Get_Hdr() && picture_coding_type != I_TYPE);

	Start_Flag = true;
	process.file = d2v_current.file;
	process.lba = d2v_current.lba;

	Decode_Picture();

	process.op = process.mi = timeGetTime();

	while (Get_Hdr())
	{
		Decode_Picture();

		if (Stop_Flag)
		{
			Fault_Flag = 99;
			Write_Frame(NULL, d2v_current, 0);
		}
	}

	return 0;
}

static BOOL GOPBack()
{
	int i;
	int startfile;
	__int64 startloc, endloc, curloc, startlba;

	startfile = process.startfile;
	startloc = process.startloc;
	startlba = process.lba;

	for (;;)
	{
		endloc = startloc;
		startloc -= BUFFER_SIZE<<4;

		if (startloc >= 0)
		{
			process.startfile = startfile;
			process.startloc = startloc;
		}
		else
		{
			process.startloc = 0;
			return false;
		}

		_lseeki64(Infile[process.startfile], process.startloc, SEEK_SET);
		Initialize_Buffer();

		while (true)
		{
			curloc = _telli64(Infile[process.startfile]);
			if (curloc >= endloc) break;
			if (!Get_Hdr()) break;
			if (picture_coding_type==I_TYPE)
			{
				if (d2v_current.file > startfile)
				{
					// We've crossed back into the next file and found
					// the first I frame. That's not the one we are looking for,
					// so go back again and keep looking.
					File_Flag = startfile;
					process.run = 0;
					for (i=0; i < startfile; i++)
						process.run += process.length[i];
					break;
				}
				if (d2v_current.lba == startlba)
				{
					// We've found the I frame we are trying to step back from.
					// That's not the one we want, so keep looking.
					break;
				}
				// We've found the I frame we want!
				process.startfile = d2v_current.file;
				process.startloc = d2v_current.lba * BUFFER_SIZE;
				// This is a kludge. For PES streams, the pack start
				// might be in the previous LBA!
				if (SystemStream_Flag)
					process.startloc -= BUFFER_SIZE;
				return true;
			}
		}
	}
}

static int ChromaFormat[4] = {
	0, 6, 8, 12
};

static void InitialDecoder()
{
	int i, size;
	extern int Clip_Width, Clip_Height;

	mb_width = (horizontal_size+15)/16;
	mb_height = progressive_sequence ? (vertical_size+15)/16 : 2*((vertical_size+31)/32);

	Coded_Picture_Width = 16 * mb_width;
	Coded_Picture_Height = 16 * mb_height;

	Chroma_Width = (chroma_format==CHROMA444) ? Coded_Picture_Width : Coded_Picture_Width>>1;
	Chroma_Height = (chroma_format!=CHROMA420) ? Coded_Picture_Height : Coded_Picture_Height>>1;

	block_count = ChromaFormat[chroma_format];

	for (i=0; i<3; i++)
	{
		if (i==0)
			size = Coded_Picture_Width * Coded_Picture_Height;
		else
			size = Chroma_Width * Chroma_Height;

		backward_reference_frame[i] = (unsigned char*)_aligned_malloc(size, 64);
		forward_reference_frame[i] = (unsigned char*)_aligned_malloc(size, 64);
		auxframe[i] = (unsigned char*)_aligned_malloc(size, 64);
	}

	u422 = (unsigned char*)_aligned_malloc(Coded_Picture_Width*Coded_Picture_Height/2, 64);
	v422 = (unsigned char*)_aligned_malloc(Coded_Picture_Width*Coded_Picture_Height/2, 64);
	u444 = (unsigned char*)_aligned_malloc(Coded_Picture_Width*Coded_Picture_Height, 64);
	v444 = (unsigned char*)_aligned_malloc(Coded_Picture_Width*Coded_Picture_Height, 64);
	rgb24 = (unsigned char*)_aligned_malloc(Coded_Picture_Width*Coded_Picture_Height*3, 64);
	rgb24small = (unsigned char*)_aligned_malloc(Coded_Picture_Width*Coded_Picture_Height*3, 64);
	yuy2 = (unsigned char*)_aligned_malloc(Coded_Picture_Width*Coded_Picture_Height*2, 64);
	lum = (unsigned char*)_aligned_malloc(Coded_Picture_Width*Coded_Picture_Height, 64);
	Clip_Width = Coded_Picture_Width;
	Clip_Height = Coded_Picture_Height;

	CheckDirectDraw();
}