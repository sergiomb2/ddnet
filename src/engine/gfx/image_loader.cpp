#include "image_loader.h"
#include <base/log.h>
#include <base/system.h>
#include <csetjmp>
#include <cstdlib>

#include <png.h>

struct SLibPNGWarningItem
{
	SImageByteBuffer *m_pByteLoader;
	const char *m_pFileName;
	std::jmp_buf m_Buf;
};

[[noreturn]] static void LibPNGError(png_structp png_ptr, png_const_charp error_msg)
{
	SLibPNGWarningItem *pUserStruct = (SLibPNGWarningItem *)png_get_error_ptr(png_ptr);
	pUserStruct->m_pByteLoader->m_Err = -1;
	dbg_msg("png", "error for file \"%s\": %s", pUserStruct->m_pFileName, error_msg);
	std::longjmp(pUserStruct->m_Buf, 1);
}

static void LibPNGWarning(png_structp png_ptr, png_const_charp warning_msg)
{
	SLibPNGWarningItem *pUserStruct = (SLibPNGWarningItem *)png_get_error_ptr(png_ptr);
	dbg_msg("png", "warning for file \"%s\": %s", pUserStruct->m_pFileName, warning_msg);
}

static bool FileMatchesImageType(SImageByteBuffer &ByteLoader)
{
	if(ByteLoader.m_pvLoadedImageBytes->size() >= 8)
		return png_sig_cmp((png_bytep)ByteLoader.m_pvLoadedImageBytes->data(), 0, 8) == 0;
	return false;
}

static void ReadDataFromLoadedBytes(png_structp pPNGStruct, png_bytep pOutBytes, png_size_t ByteCountToRead)
{
	png_voidp pIO_Ptr = png_get_io_ptr(pPNGStruct);

	SImageByteBuffer *pByteLoader = (SImageByteBuffer *)pIO_Ptr;

	if(pByteLoader->m_pvLoadedImageBytes->size() >= pByteLoader->m_LoadOffset + (size_t)ByteCountToRead)
	{
		mem_copy(pOutBytes, &(*pByteLoader->m_pvLoadedImageBytes)[pByteLoader->m_LoadOffset], (size_t)ByteCountToRead);

		pByteLoader->m_LoadOffset += (size_t)ByteCountToRead;
	}
	else
	{
		pByteLoader->m_Err = -1;
		dbg_msg("png", "could not read bytes, file was too small.");
	}
}

static EImageFormat LibPNGGetImageFormat(int ColorChannelCount)
{
	switch(ColorChannelCount)
	{
	case 1:
		return IMAGE_FORMAT_R;
	case 2:
		return IMAGE_FORMAT_RA;
	case 3:
		return IMAGE_FORMAT_RGB;
	case 4:
		return IMAGE_FORMAT_RGBA;
	default:
		dbg_assert(false, "ColorChannelCount invalid");
		dbg_break();
	}
}

static void LibPNGDeleteReadStruct(png_structp pPNGStruct, png_infop pPNGInfo)
{
	if(pPNGInfo != nullptr)
		png_destroy_info_struct(pPNGStruct, &pPNGInfo);
	png_destroy_read_struct(&pPNGStruct, nullptr, nullptr);
}

static int PngliteIncompatibility(png_structp pPNGStruct, png_infop pPNGInfo)
{
	int ColorType = png_get_color_type(pPNGStruct, pPNGInfo);
	int BitDepth = png_get_bit_depth(pPNGStruct, pPNGInfo);
	int InterlaceType = png_get_interlace_type(pPNGStruct, pPNGInfo);
	int Result = 0;
	switch(ColorType)
	{
	case PNG_COLOR_TYPE_GRAY:
	case PNG_COLOR_TYPE_RGB:
	case PNG_COLOR_TYPE_RGB_ALPHA:
	case PNG_COLOR_TYPE_GRAY_ALPHA:
		break;
	default:
		log_debug("png", "color type %d unsupported by pnglite", ColorType);
		Result |= PNGLITE_COLOR_TYPE;
	}

	switch(BitDepth)
	{
	case 8:
	case 16:
		break;
	default:
		log_debug("png", "bit depth %d unsupported by pnglite", BitDepth);
		Result |= PNGLITE_BIT_DEPTH;
	}

	if(InterlaceType != PNG_INTERLACE_NONE)
	{
		log_debug("png", "interlace type %d unsupported by pnglite", InterlaceType);
		Result |= PNGLITE_INTERLACE_TYPE;
	}
	if(png_get_compression_type(pPNGStruct, pPNGInfo) != PNG_COMPRESSION_TYPE_BASE)
	{
		log_debug("png", "non-default compression type unsupported by pnglite");
		Result |= PNGLITE_COMPRESSION_TYPE;
	}
	if(png_get_filter_type(pPNGStruct, pPNGInfo) != PNG_FILTER_TYPE_BASE)
	{
		log_debug("png", "non-default filter type unsupported by pnglite");
		Result |= PNGLITE_FILTER_TYPE;
	}
	return Result;
}

bool LoadPng(SImageByteBuffer &ByteLoader, const char *pFileName, int &PngliteIncompatible, size_t &Width, size_t &Height, uint8_t *&pImageBuff, EImageFormat &ImageFormat)
{
	SLibPNGWarningItem UserErrorStruct = {&ByteLoader, pFileName, {}};

	png_structp pPNGStruct = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);

	if(pPNGStruct == nullptr)
	{
		dbg_msg("png", "libpng internal failure: png_create_read_struct failed.");
		return false;
	}

	png_infop pPNGInfo = nullptr;
	png_bytepp pRowPointers = nullptr;
	Height = 0; // ensure this is not undefined for the error handler
	if(setjmp(UserErrorStruct.m_Buf))
	{
		if(pRowPointers != nullptr)
		{
			for(size_t i = 0; i < Height; ++i)
			{
				delete[] pRowPointers[i];
			}
		}
		delete[] pRowPointers;
		LibPNGDeleteReadStruct(pPNGStruct, pPNGInfo);
		return false;
	}
	png_set_error_fn(pPNGStruct, &UserErrorStruct, LibPNGError, LibPNGWarning);

	pPNGInfo = png_create_info_struct(pPNGStruct);

	if(pPNGInfo == nullptr)
	{
		png_destroy_read_struct(&pPNGStruct, nullptr, nullptr);
		dbg_msg("png", "libpng internal failure: png_create_info_struct failed.");
		return false;
	}

	if(!FileMatchesImageType(ByteLoader))
	{
		LibPNGDeleteReadStruct(pPNGStruct, pPNGInfo);
		dbg_msg("png", "file does not match image type.");
		return false;
	}

	ByteLoader.m_LoadOffset = 8;

	png_set_read_fn(pPNGStruct, (png_bytep)&ByteLoader, ReadDataFromLoadedBytes);

	png_set_sig_bytes(pPNGStruct, 8);

	png_read_info(pPNGStruct, pPNGInfo);

	if(ByteLoader.m_Err != 0)
	{
		LibPNGDeleteReadStruct(pPNGStruct, pPNGInfo);
		dbg_msg("png", "byte loader error.");
		return false;
	}

	Width = png_get_image_width(pPNGStruct, pPNGInfo);
	Height = png_get_image_height(pPNGStruct, pPNGInfo);
	const int ColorType = png_get_color_type(pPNGStruct, pPNGInfo);
	const png_byte BitDepth = png_get_bit_depth(pPNGStruct, pPNGInfo);
	PngliteIncompatible = PngliteIncompatibility(pPNGStruct, pPNGInfo);

	if(BitDepth == 16)
	{
		png_set_strip_16(pPNGStruct);
	}
	else if(BitDepth > 8)
	{
		dbg_msg("png", "non supported bit depth.");
		LibPNGDeleteReadStruct(pPNGStruct, pPNGInfo);
		return false;
	}

	if(Width == 0 || Height == 0 || BitDepth == 0)
	{
		dbg_msg("png", "image had width, height or bit depth of 0.");
		LibPNGDeleteReadStruct(pPNGStruct, pPNGInfo);
		return false;
	}

	if(ColorType == PNG_COLOR_TYPE_PALETTE)
		png_set_palette_to_rgb(pPNGStruct);

	if(ColorType == PNG_COLOR_TYPE_GRAY && BitDepth < 8)
		png_set_expand_gray_1_2_4_to_8(pPNGStruct);

	if(png_get_valid(pPNGStruct, pPNGInfo, PNG_INFO_tRNS))
		png_set_tRNS_to_alpha(pPNGStruct);

	png_read_update_info(pPNGStruct, pPNGInfo);

	const size_t ColorChannelCount = png_get_channels(pPNGStruct, pPNGInfo);
	const size_t BytesInRow = png_get_rowbytes(pPNGStruct, pPNGInfo);
	dbg_assert(BytesInRow == Width * ColorChannelCount, "bytes in row incorrect.");

	pRowPointers = new png_bytep[Height];
	for(size_t y = 0; y < Height; ++y)
	{
		pRowPointers[y] = new png_byte[BytesInRow];
	}

	png_read_image(pPNGStruct, pRowPointers);

	if(ByteLoader.m_Err == 0)
		pImageBuff = (uint8_t *)malloc(Height * Width * ColorChannelCount * sizeof(uint8_t));

	for(size_t i = 0; i < Height; ++i)
	{
		if(ByteLoader.m_Err == 0)
			mem_copy(&pImageBuff[i * BytesInRow], pRowPointers[i], BytesInRow);
		delete[] pRowPointers[i];
	}
	delete[] pRowPointers;
	pRowPointers = nullptr;

	if(ByteLoader.m_Err != 0)
	{
		LibPNGDeleteReadStruct(pPNGStruct, pPNGInfo);
		dbg_msg("png", "byte loader error.");
		return false;
	}

	ImageFormat = LibPNGGetImageFormat(ColorChannelCount);

	png_destroy_info_struct(pPNGStruct, &pPNGInfo);
	png_destroy_read_struct(&pPNGStruct, nullptr, nullptr);

	return true;
}

static void WriteDataFromLoadedBytes(png_structp pPNGStruct, png_bytep pOutBytes, png_size_t ByteCountToWrite)
{
	if(ByteCountToWrite > 0)
	{
		png_voidp pIO_Ptr = png_get_io_ptr(pPNGStruct);

		SImageByteBuffer *pByteLoader = (SImageByteBuffer *)pIO_Ptr;

		size_t NewSize = pByteLoader->m_LoadOffset + (size_t)ByteCountToWrite;
		pByteLoader->m_pvLoadedImageBytes->resize(NewSize);

		mem_copy(&(*pByteLoader->m_pvLoadedImageBytes)[pByteLoader->m_LoadOffset], pOutBytes, (size_t)ByteCountToWrite);
		pByteLoader->m_LoadOffset = NewSize;
	}
}

static void FlushPNGWrite(png_structp png_ptr) {}

static size_t ImageLoaderHelperFormatToColorChannel(EImageFormat Format)
{
	switch(Format)
	{
	case IMAGE_FORMAT_R:
		return 1;
	case IMAGE_FORMAT_RA:
		return 2;
	case IMAGE_FORMAT_RGB:
		return 3;
	case IMAGE_FORMAT_RGBA:
		return 4;
	default:
		dbg_assert(false, "Format invalid");
		dbg_break();
	}
}

bool SavePng(EImageFormat ImageFormat, const uint8_t *pRawBuffer, SImageByteBuffer &WrittenBytes, size_t Width, size_t Height)
{
	png_structp pPNGStruct = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);

	if(pPNGStruct == nullptr)
	{
		dbg_msg("png", "libpng internal failure: png_create_write_struct failed.");
		return false;
	}

	png_infop pPNGInfo = png_create_info_struct(pPNGStruct);

	if(pPNGInfo == nullptr)
	{
		png_destroy_read_struct(&pPNGStruct, nullptr, nullptr);
		dbg_msg("png", "libpng internal failure: png_create_info_struct failed.");
		return false;
	}

	WrittenBytes.m_LoadOffset = 0;
	WrittenBytes.m_pvLoadedImageBytes->clear();

	png_set_write_fn(pPNGStruct, (png_bytep)&WrittenBytes, WriteDataFromLoadedBytes, FlushPNGWrite);

	int ColorType = PNG_COLOR_TYPE_RGB;
	size_t WriteBytesPerPixel = ImageLoaderHelperFormatToColorChannel(ImageFormat);
	if(ImageFormat == IMAGE_FORMAT_R)
	{
		ColorType = PNG_COLOR_TYPE_GRAY;
	}
	else if(ImageFormat == IMAGE_FORMAT_RGBA)
	{
		ColorType = PNG_COLOR_TYPE_RGBA;
	}

	png_set_IHDR(pPNGStruct, pPNGInfo, Width, Height, 8, ColorType, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);

	png_write_info(pPNGStruct, pPNGInfo);

	png_bytepp pRowPointers = new png_bytep[Height];
	size_t WidthBytes = Width * WriteBytesPerPixel;
	ptrdiff_t BufferOffset = 0;
	for(size_t y = 0; y < Height; ++y)
	{
		pRowPointers[y] = new png_byte[WidthBytes];
		mem_copy(pRowPointers[y], pRawBuffer + BufferOffset, WidthBytes);
		BufferOffset += (ptrdiff_t)WidthBytes;
	}
	png_write_image(pPNGStruct, pRowPointers);

	png_write_end(pPNGStruct, pPNGInfo);

	for(size_t y = 0; y < Height; ++y)
	{
		delete[](pRowPointers[y]);
	}
	delete[](pRowPointers);

	png_destroy_info_struct(pPNGStruct, &pPNGInfo);
	png_destroy_write_struct(&pPNGStruct, nullptr);

	return true;
}
