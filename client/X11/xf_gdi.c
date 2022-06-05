/**
 * FreeRDP: A Remote Desktop Protocol Client
 * X11 GDI
 *
 * Copyright 2011 Marc-Andre Moreau <marcandre.moreau@gmail.com>
 * Copyright 2012-2013 Jay Sorg <jay.sorg@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <sys/ipc.h>
#include <sys/shm.h>
#include <X11/extensions/XShm.h>

#include <freerdp/gdi/gdi.h>
#include <freerdp/codec/rfx.h>
#include <freerdp/codec/nsc.h>
#include <freerdp/constants.h>
#include <freerdp/utils/memory.h>
#include <freerdp/utils/hexdump.h>
#include <freerdp/codec/color.h>
#include <freerdp/codec/bitmap.h>
#include <freerdp/codec/jpeg.h>

#include "xf_gdi.h"

#ifdef WITH_YAMIINF
#include <unistd.h> /* close */
#include <xcb/dri3.h>
#include YAMIINF_INC_FILE
extern struct yami_funcs g_yami_funcs; /* in xfreerdp.c */
#endif

#define LLOG_LEVEL 1
#define LLOGLN(_level, _args) \
  do { if (_level < LLOG_LEVEL) { printf _args ; printf("\n"); } } while (0)
#define LHEXDUMP(_level, _args) \
  do { if (_level < LLOG_LEVEL) { freerdp_hexdump _args ; } } while (0)

static const uint8 xf_rop2_table[] =
{
	0,
	GXclear,        /* 0 */
	GXnor,          /* DPon */
	GXandInverted,  /* DPna */
	GXcopyInverted, /* Pn */
	GXandReverse,   /* PDna */
	GXinvert,       /* Dn */
	GXxor,          /* DPx */
	GXnand,         /* DPan */
	GXand,          /* DPa */
	GXequiv,        /* DPxn */
	GXnoop,         /* D */
	GXorInverted,   /* DPno */
	GXcopy,         /* P */
	GXorReverse,    /* PDno */
	GXor,           /* DPo */
	GXset           /* 1 */
};

tbool xf_set_rop2(xfInfo* xfi, int rop2)
{
	if ((rop2 < 0x01) || (rop2 > 0x10))
	{
		printf("Unsupported ROP2: %d\n", rop2);
		return false;
	}

	XSetFunction(xfi->display, xfi->gc, xf_rop2_table[rop2]);
	return true;
}

tbool xf_set_rop3(xfInfo* xfi, int rop3)
{
	int function = -1;

	switch (rop3)
	{
		case GDI_BLACKNESS:
			function = GXclear;
			break;

		case 0x000500A9:
			function = GXnor;
			break;

		case 0x000A0329:
			function = GXandInverted;
			break;

		case 0x000F0001:
			function = GXcopyInverted;
			break;

		case GDI_NOTSRCERASE:
			function = GXnor;
			break;

		case GDI_DSna:
			function = GXandInverted;
			break;

		case GDI_NOTSRCCOPY:
			function = GXcopyInverted;
			break;

		case GDI_SRCERASE:
			function = GXandReverse;
			break;

		case 0x00500325:
			function = GXandReverse;
			break;

		case GDI_DSTINVERT:
			function = GXinvert;
			break;

		case GDI_PATINVERT:
			function = GXxor;
			break;

		case 0x005F00E9:
			function = GXnand;
			break;

		case GDI_SRCINVERT:
			function = GXxor;
			break;

		case 0x007700E6:
			function = GXnand;
			break;

		case GDI_SRCAND:
			function = GXand;
			break;

		case 0x00990066:
			function = GXequiv;
			break;

		case 0x00A000C9:
			function = GXand;
			break;

		case GDI_PDxn:
			function = GXequiv;
			break;

		case 0x00AA0029:
			function = GXnoop;
			break;

		case 0x00AF0229:
			function = GXorInverted;
			break;

		case GDI_MERGEPAINT:
			function = GXorInverted;
			break;

		case GDI_SRCCOPY:
			function = GXcopy;
			break;

		case 0x00DD0228:
			function = GXorReverse;
			break;

		case GDI_SRCPAINT:
			function = GXor;
			break;

		case GDI_PATCOPY:
			function = GXcopy;
			break;

		case 0x00F50225:
			function = GXorReverse;
			break;

		case 0x00FA0089:
			function = GXor;
			break;

		case GDI_WHITENESS:
			function = GXset;
			break;

		default:
			break;
	}

	if (function < 0)
	{
		printf("Unsupported ROP3: 0x%08X\n", rop3);
		XSetFunction(xfi->display, xfi->gc, GXclear);
		return false;
	}

	XSetFunction(xfi->display, xfi->gc, function);

	return true;
}

Pixmap xf_brush_new(xfInfo* xfi, int width, int height, int bpp, uint8* data)
{
	Pixmap bitmap;
	uint8* cdata;
	XImage* image;

	bitmap = XCreatePixmap(xfi->display, xfi->drawable, width, height, xfi->depth);
	if(data != NULL)
	{
		GC gc; /* FIXME, should cache */
		cdata = freerdp_image_convert(data, NULL, width, height, bpp, xfi->bpp, xfi->clrconv);
		image = XCreateImage(xfi->display, xfi->visual, xfi->depth,
				ZPixmap, 0, (char*) cdata, width, height, xfi->scanline_pad, 0);
		gc = XCreateGC(xfi->display, xfi->drawable, 0, NULL);
		XPutImage(xfi->display, bitmap, gc, image, 0, 0, 0, 0, width, height);
		XFree(image);
		if (cdata != data)
		{
			xfree(cdata);
		}
		XFreeGC(xfi->display, gc);
	}
	return bitmap;
}

Pixmap xf_mono_bitmap_new(xfInfo* xfi, int width, int height, uint8* data)
{
	int scanline;
	XImage* image;
	Pixmap bitmap;

	scanline = (width + 7) / 8;
	bitmap = XCreatePixmap(xfi->display, xfi->drawable, width, height, 1);
	image = XCreateImage(xfi->display, xfi->visual, 1,
			ZPixmap, 0, (char*) data, width, height, 8, scanline);
	XPutImage(xfi->display, bitmap, xfi->gc_mono, image, 0, 0, 0, 0, width, height);
	XFree(image);
	return bitmap;
}

Pixmap xf_glyph_new(xfInfo* xfi, int width, int height, uint8* data)
{
	int scanline;
	Pixmap bitmap;
	XImage* image;

	scanline = (width + 7) / 8;
	bitmap = XCreatePixmap(xfi->display, xfi->drawable, width, height, 1);
	image = XCreateImage(xfi->display, xfi->visual, 1,
			ZPixmap, 0, (char*) data, width, height, 8, scanline);
	image->byte_order = MSBFirst;
	image->bitmap_bit_order = MSBFirst;
	XInitImage(image);
	XPutImage(xfi->display, bitmap, xfi->gc_mono, image, 0, 0, 0, 0, width, height);
	XFree(image);
	return bitmap;
}

void xf_gdi_palette_update(rdpContext* context, PALETTE_UPDATE* palette)
{
	xfInfo* xfi = ((xfContext*) context)->xfi;
	xfi->clrconv->palette->count = palette->number;
	xfi->clrconv->palette->entries = palette->entries;
}

void xf_gdi_set_bounds(rdpContext* context, rdpBounds* bounds)
{
	XRectangle clip;
	xfInfo* xfi = ((xfContext*) context)->xfi;

	if (bounds != NULL)
	{
		clip.x = bounds->left;
		clip.y = bounds->top;
		clip.width = bounds->right - bounds->left + 1;
		clip.height = bounds->bottom - bounds->top + 1;
		XSetClipRectangles(xfi->display, xfi->gc, 0, 0, &clip, 1, YXBanded);
	}
	else
	{
		XSetClipMask(xfi->display, xfi->gc, None);
	}
}

void xf_gdi_dstblt(rdpContext* context, DSTBLT_ORDER* dstblt)
{
	Drawable dst;
	xfInfo* xfi = ((xfContext*) context)->xfi;

	xf_set_rop3(xfi, gdi_rop3_code(dstblt->bRop));
	GET_DST(xfi, dst);
	XSetFillStyle(xfi->display, xfi->gc, FillSolid);
	XFillRectangle(xfi->display, dst, xfi->gc,
			dstblt->nLeftRect, dstblt->nTopRect,
			dstblt->nWidth, dstblt->nHeight);
	if (xfi->drawing == xfi->primary)
	{
		if (!xfi->remote_app && !xfi->skip_bs)
		{
			XFillRectangle(xfi->display, xfi->drawable, xfi->gc,
					dstblt->nLeftRect, dstblt->nTopRect,
					dstblt->nWidth, dstblt->nHeight);
		}
		gdi_InvalidateRegion(xfi->hdc, dstblt->nLeftRect, dstblt->nTopRect,
				dstblt->nWidth, dstblt->nHeight);
	}
	XSetFunction(xfi->display, xfi->gc, GXcopy);
}

void xf_gdi_patblt(rdpContext* context, PATBLT_ORDER* patblt)
{
	Drawable dst;
	Pixmap pattern;
	rdpBrush* brush;
	uint32 foreColor;
	uint32 backColor;
	xfInfo* xfi = ((xfContext*) context)->xfi;

	GET_DST(xfi, dst);
	brush = &patblt->brush;
	xf_set_rop3(xfi, gdi_rop3_code(patblt->bRop));
	foreColor = freerdp_color_convert_rgb(patblt->foreColor, xfi->srcBpp, 32, xfi->clrconv);
	backColor = freerdp_color_convert_rgb(patblt->backColor, xfi->srcBpp, 32, xfi->clrconv);
	if (brush->style == GDI_BS_SOLID)
	{
		XSetFillStyle(xfi->display, xfi->gc, FillSolid);
		XSetForeground(xfi->display, xfi->gc, foreColor);
		XFillRectangle(xfi->display, dst, xfi->gc,
				patblt->nLeftRect, patblt->nTopRect,
				patblt->nWidth, patblt->nHeight);
	}
	else if (brush->style == GDI_BS_PATTERN)
	{
		if (brush->bpp > 1)
		{
			pattern = xf_brush_new(xfi, 8, 8, brush->bpp, brush->data);
			XSetFillStyle(xfi->display, xfi->gc, FillTiled);
			XSetTile(xfi->display, xfi->gc, pattern);
			XSetTSOrigin(xfi->display, xfi->gc, brush->x, brush->y);
			XFillRectangle(xfi->display, dst, xfi->gc,
					patblt->nLeftRect, patblt->nTopRect,
					patblt->nWidth, patblt->nHeight);
			XSetTile(xfi->display, xfi->gc, xfi->primary);
			XFreePixmap(xfi->display, pattern);
		}
		else
		{
			pattern = xf_mono_bitmap_new(xfi, 8, 8, brush->data);
			XSetForeground(xfi->display, xfi->gc, foreColor);
			XSetBackground(xfi->display, xfi->gc, backColor);
			XSetFillStyle(xfi->display, xfi->gc, FillOpaqueStippled);
			XSetStipple(xfi->display, xfi->gc, pattern);
			XSetTSOrigin(xfi->display, xfi->gc, brush->x, brush->y);
			XFillRectangle(xfi->display, dst, xfi->gc,
					patblt->nLeftRect, patblt->nTopRect,
					patblt->nWidth, patblt->nHeight);
			XFreePixmap(xfi->display, pattern);
		}
	}
	else
	{
		printf("unimplemented brush style:%d\n", brush->style);
	}
	if (xfi->drawing == xfi->primary)
	{
		XSetFunction(xfi->display, xfi->gc, GXcopy);
		if (!xfi->remote_app && !xfi->skip_bs)
		{
			XCopyArea(xfi->display, xfi->primary, xfi->drawable, xfi->gc,
				patblt->nLeftRect, patblt->nTopRect,
				patblt->nWidth, patblt->nHeight,
				patblt->nLeftRect, patblt->nTopRect);
		}
		gdi_InvalidateRegion(xfi->hdc, patblt->nLeftRect, patblt->nTopRect,
				patblt->nWidth, patblt->nHeight);
	}
	XSetFunction(xfi->display, xfi->gc, GXcopy);
}

void xf_gdi_scrblt(rdpContext* context, SCRBLT_ORDER* scrblt)
{
	xfInfo* xfi = ((xfContext*) context)->xfi;

	xf_set_rop3(xfi, gdi_rop3_code(scrblt->bRop));
	if (xfi->skip_bs)
	{
		if (xfi->drawing == xfi->primary)
		{
			XCopyArea(xfi->display, xfi->drawable, xfi->drawable, xfi->gc,
					scrblt->nXSrc, scrblt->nYSrc,
					scrblt->nWidth, scrblt->nHeight,
					scrblt->nLeftRect, scrblt->nTopRect);
			gdi_InvalidateRegion(xfi->hdc, scrblt->nLeftRect, scrblt->nTopRect,
					scrblt->nWidth, scrblt->nHeight);
		}
		else
		{
			XCopyArea(xfi->display, xfi->drawable, xfi->drawing, xfi->gc,
					scrblt->nXSrc, scrblt->nYSrc,
					scrblt->nWidth, scrblt->nHeight,
					scrblt->nLeftRect, scrblt->nTopRect);
		}
		XSetFunction(xfi->display, xfi->gc, GXcopy);
		return;
	}
	XCopyArea(xfi->display, xfi->primary, xfi->drawing, xfi->gc, scrblt->nXSrc, scrblt->nYSrc,
			scrblt->nWidth, scrblt->nHeight, scrblt->nLeftRect, scrblt->nTopRect);
	if (xfi->drawing == xfi->primary)
	{
		if (!xfi->remote_app)
		{
			if (xfi->unobscured)
			{
				XCopyArea(xfi->display, xfi->drawable, xfi->drawable, xfi->gc,
						scrblt->nXSrc, scrblt->nYSrc,
						scrblt->nWidth, scrblt->nHeight,
						scrblt->nLeftRect, scrblt->nTopRect);
			}
			else
			{
				XSetFunction(xfi->display, xfi->gc, GXcopy);
				XCopyArea(xfi->display, xfi->primary, xfi->drawable, xfi->gc,
						scrblt->nLeftRect, scrblt->nTopRect,
						scrblt->nWidth, scrblt->nHeight,
						scrblt->nLeftRect, scrblt->nTopRect);
			}
		}
		gdi_InvalidateRegion(xfi->hdc, scrblt->nLeftRect, scrblt->nTopRect,
				scrblt->nWidth, scrblt->nHeight);
	}
	XSetFunction(xfi->display, xfi->gc, GXcopy);
}

void xf_gdi_opaque_rect(rdpContext* context, OPAQUE_RECT_ORDER* opaque_rect)
{
	Drawable dst;
	uint32 color;
	xfInfo* xfi = ((xfContext*) context)->xfi;

	color = freerdp_color_convert_var(opaque_rect->color, xfi->srcBpp, 32, xfi->clrconv);
	XSetFunction(xfi->display, xfi->gc, GXcopy);
	XSetFillStyle(xfi->display, xfi->gc, FillSolid);
	XSetForeground(xfi->display, xfi->gc, color);
	GET_DST(xfi, dst);
	XFillRectangle(xfi->display, dst, xfi->gc,
			opaque_rect->nLeftRect, opaque_rect->nTopRect,
			opaque_rect->nWidth, opaque_rect->nHeight);
	if (xfi->drawing == xfi->primary)
	{
		if (!xfi->remote_app && !xfi->skip_bs)
		{
			XFillRectangle(xfi->display, xfi->drawable, xfi->gc,
					opaque_rect->nLeftRect, opaque_rect->nTopRect,
					opaque_rect->nWidth, opaque_rect->nHeight);
		}
		gdi_InvalidateRegion(xfi->hdc, opaque_rect->nLeftRect, opaque_rect->nTopRect,
				opaque_rect->nWidth, opaque_rect->nHeight);
	}
}

void xf_gdi_multi_opaque_rect(rdpContext* context, MULTI_OPAQUE_RECT_ORDER* multi_opaque_rect)
{
	Drawable dst;
	int i;
	uint32 color;
	DELTA_RECT* rectangle;
	xfInfo* xfi = ((xfContext*) context)->xfi;

	color = freerdp_color_convert_var(multi_opaque_rect->color, xfi->srcBpp, 32, xfi->clrconv);
	XSetFunction(xfi->display, xfi->gc, GXcopy);
	XSetFillStyle(xfi->display, xfi->gc, FillSolid);
	XSetForeground(xfi->display, xfi->gc, color);
	GET_DST(xfi, dst);
	for (i = 1; i < multi_opaque_rect->numRectangles + 1; i++)
	{
		rectangle = &multi_opaque_rect->rectangles[i];
		XFillRectangle(xfi->display, dst, xfi->gc,
				rectangle->left, rectangle->top,
				rectangle->width, rectangle->height);
		if (xfi->drawing == xfi->primary)
		{
			if (!xfi->remote_app && !xfi->skip_bs)
			{
				XFillRectangle(xfi->display, xfi->drawable, xfi->gc,
					rectangle->left, rectangle->top,
					rectangle->width, rectangle->height);
			}
			gdi_InvalidateRegion(xfi->hdc, rectangle->left, rectangle->top,
					rectangle->width, rectangle->height);
		}
	}
}

void xf_gdi_line_to(rdpContext* context, LINE_TO_ORDER* line_to)
{
	Drawable dst;
	uint32 color;
	xfInfo* xfi = ((xfContext*) context)->xfi;

	xf_set_rop2(xfi, line_to->bRop2);
	color = freerdp_color_convert_rgb(line_to->penColor, xfi->srcBpp, 32, xfi->clrconv);

	XSetFillStyle(xfi->display, xfi->gc, FillSolid);
	XSetForeground(xfi->display, xfi->gc, color);
	GET_DST(xfi, dst);
	XDrawLine(xfi->display, dst, xfi->gc,
			line_to->nXStart, line_to->nYStart, line_to->nXEnd, line_to->nYEnd);
	if (xfi->drawing == xfi->primary)
	{
		int width, height;
		if (!xfi->remote_app && !xfi->skip_bs)
		{
			XDrawLine(xfi->display, xfi->drawable, xfi->gc,
				line_to->nXStart, line_to->nYStart, line_to->nXEnd,
				line_to->nYEnd);
		}
		width = line_to->nXStart - line_to->nXEnd;
		height = line_to->nYStart - line_to->nYEnd;
		if (width < 0)
			width *= (-1);
		if (height < 0)
			height *= (-1);
		gdi_InvalidateRegion(xfi->hdc, line_to->nXStart, line_to->nYStart, width, height);
	}
	XSetFunction(xfi->display, xfi->gc, GXcopy);
}

void xf_gdi_polyline(rdpContext* context, POLYLINE_ORDER* polyline)
{
	Drawable dst;
	int i;
	int x, y;
	int x1, y1;
	int x2, y2;
	int npoints;
	uint32 color;
	XPoint* points;
	int width, height;
	xfInfo* xfi = ((xfContext*) context)->xfi;

	xf_set_rop2(xfi, polyline->bRop2);
	color = freerdp_color_convert_rgb(polyline->penColor, xfi->srcBpp, 32, xfi->clrconv);
	XSetFillStyle(xfi->display, xfi->gc, FillSolid);
	XSetForeground(xfi->display, xfi->gc, color);
	npoints = polyline->numPoints + 1;
	points = xmalloc(sizeof(XPoint) * npoints);
	points[0].x = polyline->xStart;
	points[0].y = polyline->yStart;
	for (i = 0; i < polyline->numPoints; i++)
	{
		points[i + 1].x = polyline->points[i].x;
		points[i + 1].y = polyline->points[i].y;
	}
	GET_DST(xfi, dst);
	XDrawLines(xfi->display, dst, xfi->gc, points, npoints, CoordModePrevious);
	if (xfi->drawing == xfi->primary)
	{
		if (!xfi->remote_app && !xfi->skip_bs)
		{
			XDrawLines(xfi->display, xfi->drawable, xfi->gc, points,
					npoints, CoordModePrevious);
		}
		x1 = points[0].x;
		y1 = points[0].y;
		for (i = 1; i < npoints; i++)
		{
			x2 = points[i].x + x1;
			y2 = points[i].y + y1;
			x = (x2 < x1) ? x2 : x1;
			width = (x2 > x1) ? x2 - x1 : x1 - x2;
			y = (y2 < y1) ? y2 : y1;
			height = (y2 > y1) ? y2 - y1 : y1 - y2;
			x1 = x2;
			y1 = y2;
			gdi_InvalidateRegion(xfi->hdc, x, y, width, height);
		}
	}
	XSetFunction(xfi->display, xfi->gc, GXcopy);
	xfree(points);
}

void xf_gdi_memblt(rdpContext* context, MEMBLT_ORDER* memblt)
{
	xfBitmap* bitmap;
	xfInfo* xfi = ((xfContext*) context)->xfi;

	bitmap = (xfBitmap*) memblt->bitmap;
	xf_set_rop3(xfi, gdi_rop3_code(memblt->bRop));
	if (xfi->skip_bs)
	{
		if (xfi->drawing == xfi->primary)
		{
			XCopyArea(xfi->display, bitmap->pixmap, xfi->drawable, xfi->gc,
					memblt->nXSrc, memblt->nYSrc,
					memblt->nWidth, memblt->nHeight,
					memblt->nLeftRect, memblt->nTopRect);
			gdi_InvalidateRegion(xfi->hdc, memblt->nLeftRect, memblt->nTopRect,
					memblt->nWidth, memblt->nHeight);
		}
		else
		{
			XCopyArea(xfi->display, bitmap->pixmap, xfi->drawing, xfi->gc,
					memblt->nXSrc, memblt->nYSrc,
					memblt->nWidth, memblt->nHeight,
					memblt->nLeftRect, memblt->nTopRect);
		}
	}
	else
	{
		XCopyArea(xfi->display, bitmap->pixmap, xfi->drawing, xfi->gc,
				memblt->nXSrc, memblt->nYSrc, memblt->nWidth, memblt->nHeight,
				memblt->nLeftRect, memblt->nTopRect);
		if (xfi->drawing == xfi->primary)
		{
			if (!xfi->remote_app)
			{
				XCopyArea(xfi->display, bitmap->pixmap, xfi->drawable, xfi->gc,
					memblt->nXSrc, memblt->nYSrc,
					memblt->nWidth, memblt->nHeight,
					memblt->nLeftRect, memblt->nTopRect);
			}
			gdi_InvalidateRegion(xfi->hdc, memblt->nLeftRect, memblt->nTopRect,
					memblt->nWidth, memblt->nHeight);
		}
	}
	XSetFunction(xfi->display, xfi->gc, GXcopy);
}

void xf_gdi_mem3blt(rdpContext* context, MEM3BLT_ORDER* mem3blt)
{

}

void xf_gdi_surface_frame_marker(rdpContext* context, SURFACE_FRAME_MARKER* surface_frame_marker)
{
	xfInfo* xfi;

	LLOGLN(10, ("xf_gdi_surface_frame_marker: action %d", surface_frame_marker->frameAction));
	xfi = ((xfContext*) context)->xfi;
	if (surface_frame_marker->frameAction == 0) /* begin */
	{
		xfi->frameId = surface_frame_marker->frameId;
	}
}

#ifdef WITH_YAMIINF
static int xf_gdi_surface_h264(xfInfo* xfi, SURFACE_BITS_COMMAND* surface_bits_command)
{
	STREAM* s;
	int error;
	int flags;
	int session_id;
	int src_width;
	int src_height;
	int dst_width;
	int dst_height;
	int num_rects;
	int h264_bytes;
	uint8* rects_p;
	void* decoder;
	xcb_pixmap_t pixmap = None;
	xcb_void_cookie_t cookie;
	xcb_generic_error_t* xcb_error;

	if ((surface_bits_command->bitmapDataLength < 1) || (surface_bits_command->bitmapData == NULL))
	{
		return 1;
	}
	s = stream_new(0);
	stream_attach(s, surface_bits_command->bitmapData, surface_bits_command->bitmapDataLength);
	LHEXDUMP(10, (surface_bits_command->bitmapData, surface_bits_command->bitmapDataLength));
	if (stream_get_left(s) < 18)
	{
		LLOGLN(0, ("xf_gdi_surface_h264: error bytes"));
		stream_detach(s);
		stream_free(s);
		return 1;
	}
	stream_read_uint32(s, flags);
	stream_read_uint32(s, session_id);
	LLOGLN(10, ("xf_gdi_surface_h264: flags 0x%8.8x session_id %d", flags, session_id));
	stream_read_uint16(s, src_width);
	stream_read_uint16(s, src_height);
	stream_read_uint16(s, dst_width);
	stream_read_uint16(s, dst_height);
	stream_read_uint16(s, num_rects);
	if ((num_rects < 0) || (stream_get_left(s) < num_rects * 8 + 4))
	{
		LLOGLN(0, ("xf_gdi_surface_h264: error bytes"));
		stream_detach(s);
		stream_free(s);
		return 1;
	}
	stream_get_mark(s, rects_p); /* save for later */
	stream_seek(s, num_rects * 8);
	stream_read_uint32(s, h264_bytes);
	/* h264_bytes can be zero */
	if ((h264_bytes < 0) || (stream_get_left(s) < h264_bytes))
	{
		LLOGLN(0, ("xf_gdi_surface_h264: error bytes"));
		stream_detach(s);
		stream_free(s);
		return 1;
	}
	session_id &= 0xF;
	if (flags & 2) /* delete */
	{
		if (xfi->decoders[session_id] != NULL)
		{
			g_yami_funcs.yami_decoder_delete(xfi->decoders[session_id]);
			xfi->decoders[session_id] = 0;
		}
	}
	if ((h264_bytes > 0) && (num_rects > 0))
	{
		decoder = xfi->decoders[session_id];
		if (decoder == NULL)
		{
			error = g_yami_funcs.yami_decoder_create(&decoder, src_width, src_height,
					YI_TYPE_H264, YI_H264_DEC_FLAG_LOWLATENCY);
			LLOGLN(0, ("xf_gdi_surface_h264: decoder_create rv %d width %d height %d", error,
					src_width, src_height));
			if (error == YI_SUCCESS)
			{
				xfi->decoders[session_id] = decoder;
			}
			else
			{
				decoder = NULL;
			}
		}
		if (decoder != NULL)
		{
			error = g_yami_funcs.yami_decoder_decode(decoder, s->p, h264_bytes);
			LLOGLN(10, ("xf_gdi_surface_h264: yami_decoder_decode rv %d", error));
			if (error == YI_SUCCESS)
			{
				int fd;
				int fd_width;
				int fd_height;
				int fd_stride;
				int fd_size;
				int fd_bpp;
				YI_INT64 fd_time;
				error = g_yami_funcs.yami_decoder_get_fd_dst(decoder, &fd, &fd_width, &fd_height,
						&fd_stride, &fd_size, &fd_bpp, &fd_time);
				LLOGLN(10, ("xf_gdi_surface_h264: yami_decoder_get_fd_dst rv %d", error));
				if (error == YI_SUCCESS)
				{
					LLOGLN(10, ("xf_gdi_surface_h264: yami_decoder_get_fd_dst fd %d "
							"fd_width %d fd_height %d fd_stride %d fd_size %d fd_bpp %d "
							"fd_time %lld", fd, fd_width, fd_height, fd_stride, fd_size,
							fd_bpp, fd_time));
					LLOGLN(10, ("xf_gdi_surface_h264: fd_bpp %d xfi->depth %d xfi->bpp %d",
							fd_bpp, xfi->depth, xfi->bpp));
					pixmap = xcb_generate_id(xfi->xcb);
					cookie = xcb_dri3_pixmap_from_buffer(xfi->xcb, pixmap, xfi->drawable,
							fd_size, fd_width, fd_height, fd_stride, xfi->depth, xfi->bpp, fd);
					xcb_error = xcb_request_check(xfi->xcb, cookie);
					free(xcb_error);
					close(fd);
				}
				else
				{
					LLOGLN(0, ("xf_gdi_surface_h264: yami_decoder_get_fd_dst failed %d", error));
				}
			}
			else
			{
				LLOGLN(0, ("xf_gdi_surface_h264: yami_decoder_decode failed %d", error));
			}
		}
		else
		{
			LLOGLN(0, ("xf_gdi_surface_h264: error getting decoder"));
		}
	}
	if (pixmap != None)
	{
		if ((src_width == dst_width) && (src_height == dst_height))
		{
			int x;
			int y;
			int cx;
			int cy;
			int lx;
			int ly;
			int index;
			Drawable dst = xfi->skip_bs ? xfi->drawable : xfi->primary;
			/* size of rects_p checked earlier */
			stream_attach(s, rects_p, num_rects * 8);
			for (index = 0; index < num_rects; index++)
			{
				stream_read_uint16(s, x);
				stream_read_uint16(s, y);
				stream_read_uint16(s, cx);
				stream_read_uint16(s, cy);
				lx = x + surface_bits_command->destLeft;
				ly = y + surface_bits_command->destTop;
				if (lx + cx > surface_bits_command->destRight)
				{
					cx = surface_bits_command->destRight - lx;
				}
				if (ly + cy > surface_bits_command->destBottom)
				{
					cy = surface_bits_command->destBottom - ly;
				}
				if ((cx > 0) && (cy > 0))
				{
					xcb_copy_area(xfi->xcb, pixmap, dst, xfi->xcb_gc, x, y, lx, ly, cx, cy);
					if (!xfi->remote_app && !xfi->skip_bs)
					{
						xcb_copy_area(xfi->xcb, xfi->primary, xfi->drawable, xfi->xcb_gc, lx, ly, lx, ly, cx, cy);
					}
				}
			}
		}
		else
		{
			LLOGLN(0, ("xf_gdi_surface_h264: xf_gdi_surface_bits: unsupported stretch"));
		}
		xcb_free_pixmap(xfi->xcb, pixmap);
	}
	stream_detach(s);
	stream_free(s);
	return 0;
}
#else
static int xf_gdi_surface_h264(xfInfo* xfi, SURFACE_BITS_COMMAND* surface_bits_command)
{
	return 0;
}
#endif

void xf_gdi_surface_bits(rdpContext* context, SURFACE_BITS_COMMAND* surface_bits_command)
{
	int i, tx, ty;
	XImage* image;
	RFX_MESSAGE* message;
	xfInfo* xfi = ((xfContext*) context)->xfi;
	RFX_CONTEXT* rfx_context = (RFX_CONTEXT*) xfi->rfx_context;
	NSC_CONTEXT* nsc_context = (NSC_CONTEXT*) xfi->nsc_context;

	if (surface_bits_command->codecID == CODEC_ID_H264)
	{
		xf_gdi_surface_h264(xfi, surface_bits_command);
	}
	else if (surface_bits_command->codecID == CODEC_ID_JPEG)
	{
		tui8* comp;
		tbool ok;
		Drawable dst;
		int x;
		int y;
		int cx;
		int cy;
		int header_bytes;
		int bytes;
		XShmSegmentInfo shminfo;

		XSetFunction(xfi->display, xfi->gc, GXcopy);
		XSetFillStyle(xfi->display, xfi->gc, FillSolid);
		x = surface_bits_command->destLeft;
		y = surface_bits_command->destTop;
		cx = surface_bits_command->width;
		cy = surface_bits_command->height;
		header_bytes = surface_bits_command->bitmapData[0];
		header_bytes = header_bytes | (surface_bits_command->bitmapData[1] << 8);
		comp = surface_bits_command->bitmapData + (2 + header_bytes);

		bytes = cx * cy * 4;
		if (xfi->shm_info == 0)
		{
			xfi->shm_info = create_shm_info(bytes);
		}
		else if (xfi->shm_info->bytes < bytes)
		{
			delete_shm_info(xfi->shm_info);
			xfi->shm_info = create_shm_info(bytes);
		}

		bytes = surface_bits_command->bitmapDataLength - (2 + header_bytes);
		ok = jpeg_decompress(comp, xfi->shm_info->ptr, cx, cy, bytes, 32);
		if (ok)
		{
			dst = xfi->skip_bs ? xfi->drawable : xfi->primary;
			memset(&shminfo, 0, sizeof(shminfo));
			shminfo.shmid = xfi->shm_info->shmid;
			shminfo.shmaddr = xfi->shm_info->ptr;
			image = XShmCreateImage(xfi->display, xfi->visual, xfi->depth,
					ZPixmap, xfi->shm_info->ptr, &shminfo, cx, cy);
			XShmAttach(xfi->display, &shminfo);
			XShmPutImage(xfi->display, dst, xfi->gc, image, 0, 0, x, y, cx, cy, false);
			XSync(xfi->display, false);
			XShmDetach(xfi->display, &shminfo);
			XFree(image);
			if (!xfi->remote_app && !xfi->skip_bs)
			{
				XCopyArea(xfi->display, xfi->primary, xfi->drawable, xfi->gc, x, y, cx, cy, x, y);
			}
		}
		else
		{
			printf("jpeg_decompress error\n");
		}
	}
	else if (surface_bits_command->codecID == CODEC_ID_REMOTEFX)
	{
		message = rfx_process_message(rfx_context,
				surface_bits_command->bitmapData,
				surface_bits_command->bitmapDataLength);
		XSetFunction(xfi->display, xfi->gc, GXcopy);
		XSetFillStyle(xfi->display, xfi->gc, FillSolid);
		XSetClipRectangles(xfi->display, xfi->gc,
				surface_bits_command->destLeft, surface_bits_command->destTop,
				(XRectangle*) message->rects, message->num_rects, YXBanded);
		/* Draw the tiles to primary surface, each is 64x64. */
		for (i = 0; i < message->num_tiles; i++)
		{
			image = XCreateImage(xfi->display, xfi->visual, 24, ZPixmap, 0,
				(char*) message->tiles[i]->data, 64, 64, 32, 0);
			tx = message->tiles[i]->x + surface_bits_command->destLeft;
			ty = message->tiles[i]->y + surface_bits_command->destTop;
			XPutImage(xfi->display, xfi->primary, xfi->gc, image, 0, 0, tx, ty,
					64, 64);
			XFree(image);
		}
		/* Copy the updated region from backstore to the window. */
		for (i = 0; i < message->num_rects; i++)
		{
			tx = message->rects[i].x + surface_bits_command->destLeft;
			ty = message->rects[i].y + surface_bits_command->destTop;
			if (!xfi->remote_app)
			{
				XCopyArea(xfi->display, xfi->primary, xfi->drawable, xfi->gc,
						tx, ty, message->rects[i].width,
						message->rects[i].height, tx, ty);
			}
			gdi_InvalidateRegion(xfi->hdc, tx, ty, message->rects[i].width,
					message->rects[i].height);
		}
		XSetClipMask(xfi->display, xfi->gc, None);
		rfx_message_free(rfx_context, message);
	}
	else if (surface_bits_command->codecID == CODEC_ID_NSCODEC)
	{
		nsc_context->width = surface_bits_command->width;
		nsc_context->height = surface_bits_command->height;
		nsc_process_message(nsc_context, surface_bits_command->bitmapData,
				surface_bits_command->bitmapDataLength);
		XSetFunction(xfi->display, xfi->gc, GXcopy);
		XSetFillStyle(xfi->display, xfi->gc, FillSolid);
		xfi->bmp_codec_nsc = (uint8*) xrealloc(xfi->bmp_codec_nsc,
				surface_bits_command->width * surface_bits_command->height * 4);
		freerdp_image_flip(nsc_context->bmpdata, xfi->bmp_codec_nsc,
				surface_bits_command->width, surface_bits_command->height, 32);
		image = XCreateImage(xfi->display, xfi->visual, 24, ZPixmap, 0,
			(char*) xfi->bmp_codec_nsc, surface_bits_command->width,
			surface_bits_command->height, 32, 0);
		XPutImage(xfi->display, xfi->primary, xfi->gc, image, 0, 0,
				surface_bits_command->destLeft, surface_bits_command->destTop,
				surface_bits_command->width, surface_bits_command->height);
		if (!xfi->remote_app)
		{
			XCopyArea(xfi->display, xfi->primary, xfi->window->handle, xfi->gc,
				surface_bits_command->destLeft, surface_bits_command->destTop,
				surface_bits_command->width, surface_bits_command->height,
				surface_bits_command->destLeft, surface_bits_command->destTop);
		}
		gdi_InvalidateRegion(xfi->hdc, surface_bits_command->destLeft,
				surface_bits_command->destTop,
				surface_bits_command->width, surface_bits_command->height);
		XSetClipMask(xfi->display, xfi->gc, None);
		nsc_context_destroy(nsc_context);
	}
	else if (surface_bits_command->codecID == CODEC_ID_NONE)
	{
		XSetFunction(xfi->display, xfi->gc, GXcopy);
		XSetFillStyle(xfi->display, xfi->gc, FillSolid);
		xfi->bmp_codec_none = (uint8*) xrealloc(xfi->bmp_codec_none,
				surface_bits_command->width * surface_bits_command->height * 4);
		freerdp_image_flip(surface_bits_command->bitmapData, xfi->bmp_codec_none,
				surface_bits_command->width, surface_bits_command->height, 32);
		image = XCreateImage(xfi->display, xfi->visual, 24, ZPixmap, 0,
				(char*) xfi->bmp_codec_none,
				surface_bits_command->width, surface_bits_command->height, 32, 0);
		XPutImage(xfi->display, xfi->primary, xfi->gc, image, 0, 0,
				surface_bits_command->destLeft, surface_bits_command->destTop,
				surface_bits_command->width, surface_bits_command->height);
		if (!xfi->remote_app)
		{
			XCopyArea(xfi->display, xfi->primary, xfi->window->handle, xfi->gc,
				surface_bits_command->destLeft, surface_bits_command->destTop,
				surface_bits_command->width, surface_bits_command->height,
				surface_bits_command->destLeft, surface_bits_command->destTop);
		}
		gdi_InvalidateRegion(xfi->hdc, surface_bits_command->destLeft,
				surface_bits_command->destTop,
				surface_bits_command->width, surface_bits_command->height);
		XSetClipMask(xfi->display, xfi->gc, None);
	}
	else
	{
		printf("Unsupported codecID %d\n", surface_bits_command->codecID);
	}

	LLOGLN(10, ("xf_gdi_surface_bits: sending frame ack"));
	xfi->instance->SendFrameAck(xfi->instance, xfi->frameId);
}


void xf_gdi_register_update_callbacks(rdpUpdate* update)
{
	rdpPrimaryUpdate* primary = update->primary;

	update->Palette = xf_gdi_palette_update;
	update->SetBounds = xf_gdi_set_bounds;

	primary->DstBlt = xf_gdi_dstblt;
	primary->PatBlt = xf_gdi_patblt;
	primary->ScrBlt = xf_gdi_scrblt;
	primary->OpaqueRect = xf_gdi_opaque_rect;
	primary->DrawNineGrid = NULL;
	primary->MultiDstBlt = NULL;
	primary->MultiPatBlt = NULL;
	primary->MultiScrBlt = NULL;
	primary->MultiOpaqueRect = xf_gdi_multi_opaque_rect;
	primary->MultiDrawNineGrid = NULL;
	primary->LineTo = xf_gdi_line_to;
	primary->Polyline = xf_gdi_polyline;
	primary->MemBlt = xf_gdi_memblt;
	primary->Mem3Blt = xf_gdi_mem3blt;
	primary->SaveBitmap = NULL;
	primary->GlyphIndex = NULL;
	primary->FastIndex = NULL;
	primary->FastGlyph = NULL;
	primary->PolygonSC = NULL;
	primary->PolygonCB = NULL;
	primary->EllipseSC = NULL;
	primary->EllipseCB = NULL;

	update->SurfaceBits = xf_gdi_surface_bits;
	update->SurfaceFrameMarker = xf_gdi_surface_frame_marker;
}
