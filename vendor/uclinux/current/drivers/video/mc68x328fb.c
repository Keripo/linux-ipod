/*
 *  linux/drivers/video/mc68x328fb.c -- DragonBall frame buffer device
 *
 *	Copyright (C) 2003 Georges Menie
 *
 *  Support for the built in MC68x328 LCD Controller
 *
 *  This driver assumes an already configured controller (e.g. from crt0_fixed.s)
 *  Keep the code clean of board specific initialization.
 *
 *  This code has not been tested with colors, colormap management functions
 *  are minimal (no colormap data written to the 68328 registers...)
 *
 *  modified from :
 *
 *    linux/drivers/video/vfb.c -- Virtual frame buffer device
 *	  Copyright (C) 1997 Geert Uytterhoeven
 *
 *  This file is subject to the terms and conditions of the GNU General Public
 *  License. See the file COPYING in the main directory of this archive for
 *  more details.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/tty.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <asm/uaccess.h>
#include <linux/fb.h>
#include <linux/init.h>

#include <video/fbcon.h>
#include <video/fbcon-mfb.h>
#include <video/fbcon-cfb2.h>
#include <video/fbcon-cfb4.h>
#include <video/fbcon-cfb8.h>
#include <video/fbcon-cfb16.h>
#include <video/fbcon-cfb24.h>
#include <video/fbcon-cfb32.h>

#if defined(CONFIG_M68EZ328)
#include <asm/MC68EZ328.h>
#elif defined(CONFIG_M68VZ328)
#include <asm/MC68VZ328.h>
#else
#error wrong architecture for the MC68x328 Framebuffer Device
#endif

#if defined(CONFIG_FB_MC68X328_MONO01)
#define MC68X328FB_MONO_VISUAL FB_VISUAL_MONO01
#else
#define MC68X328FB_MONO_VISUAL FB_VISUAL_MONO10
#endif

static u_long videomemory, videomemorysize;
static int currcon = 0;
static struct display disp;
static struct fb_info fb_info;
static struct { u_char red, green, blue, pad; } palette[256];
static union {
#ifdef FBCON_HAS_CFB16
    u16 cfb16[16];
#endif
#ifdef FBCON_HAS_CFB24
    u32 cfb24[16];
#endif
#ifdef FBCON_HAS_CFB32
    u32 cfb32[16];
#endif
} fbcon_cmap;
static char mc68x328fb_name[16] = "DragonBall";

static struct fb_var_screeninfo mc68x328fb_default = {
    0, 0, 0, 0, 0, 0, 0, 0, /* some value are set by mc68x328fb_init() */
    {0, 8, 0}, {0, 8, 0}, {0, 8, 0}, {0, 0, 0},
    0, 0, -1, -1, 0, 20000, 64, 64, 32, 32, 64, 2,
    0, FB_VMODE_NONINTERLACED
};


    /*
     *  Interface used by the world
     */

int mc68x328fb_setup(char*);

static int mc68x328fb_get_fix(struct fb_fix_screeninfo *fix, int con,
		       struct fb_info *info);
static int mc68x328fb_get_var(struct fb_var_screeninfo *var, int con,
		       struct fb_info *info);
static int mc68x328fb_set_var(struct fb_var_screeninfo *var, int con,
		       struct fb_info *info);
static int mc68x328fb_pan_display(struct fb_var_screeninfo *var, int con,
			   struct fb_info *info);
static int mc68x328fb_get_cmap(struct fb_cmap *cmap, int kspc, int con,
			struct fb_info *info);
static int mc68x328fb_set_cmap(struct fb_cmap *cmap, int kspc, int con,
			struct fb_info *info);


    /*
     *  Interface to the low level console driver
     */

int mc68x328fb_init(void);
static int vfbcon_switch(int con, struct fb_info *info);
static int vfbcon_updatevar(int con, struct fb_info *info);
static void vfbcon_blank(int blank, struct fb_info *info);


    /*
     *  Internal routines
     */

static u_long get_line_length(int xres_virtual, int bpp);
static void mc68x328fb_encode_fix(struct fb_fix_screeninfo *fix,
			   struct fb_var_screeninfo *var);
static void set_color_bitfields(struct fb_var_screeninfo *var);
static int mc68x328fb_getcolreg(u_int regno, u_int *red, u_int *green, u_int *blue,
                         u_int *transp, struct fb_info *info);
static int mc68x328fb_setcolreg(u_int regno, u_int red, u_int green, u_int blue,
                         u_int transp, struct fb_info *info);
static void do_install_cmap(int con, struct fb_info *info);


static struct fb_ops mc68x328fb_ops = {
	owner:		THIS_MODULE,
	fb_get_fix:	mc68x328fb_get_fix,
	fb_get_var:	mc68x328fb_get_var,
	fb_set_var:	mc68x328fb_set_var,
	fb_get_cmap:	mc68x328fb_get_cmap,
	fb_set_cmap:	mc68x328fb_set_cmap,
	fb_pan_display:	mc68x328fb_pan_display,
};

    /*
     *  Get the Fixed Part of the Display
     */

static int mc68x328fb_get_fix(struct fb_fix_screeninfo *fix, int con,
		       struct fb_info *info)
{
    struct fb_var_screeninfo *var;

    if (con == -1)
	var = &mc68x328fb_default;
    else
	var = &fb_display[con].var;
    mc68x328fb_encode_fix(fix, var);
    return 0;
}


    /*
     *  Get the User Defined Part of the Display
     */

static int mc68x328fb_get_var(struct fb_var_screeninfo *var, int con,
		       struct fb_info *info)
{
    if (con == -1)
	*var = mc68x328fb_default;
    else
	*var = fb_display[con].var;
    set_color_bitfields(var);
    return 0;
}


    /*
     *  Set the User Defined Part of the Display
     */

static int mc68x328fb_set_var(struct fb_var_screeninfo *var, int con,
		       struct fb_info *info)
{
    int err, activate = var->activate;
    int oldxres, oldyres, oldvxres, oldvyres, oldbpp;
    u_long line_length;

    struct display *display;
    if (con >= 0)
	display = &fb_display[con];
    else
	display = &disp;	/* used during initialization */

    /*
     *  FB_VMODE_CONUPDATE and FB_VMODE_SMOOTH_XPAN are equal!
     *  as FB_VMODE_SMOOTH_XPAN is only used internally
     */

    if (var->vmode & FB_VMODE_CONUPDATE) {
	var->vmode |= FB_VMODE_YWRAP;
	var->xoffset = display->var.xoffset;
	var->yoffset = display->var.yoffset;
    }

    /*
     *  Some very basic checks
     */
    if (!var->xres)
	var->xres = 1;
    if (!var->yres)
	var->yres = 1;
    if (var->xres > var->xres_virtual)
	var->xres_virtual = var->xres;
    if (var->yres > var->yres_virtual)
	var->yres_virtual = var->yres;
    if (var->bits_per_pixel <= 1)
	var->bits_per_pixel = 1;
    else if (var->bits_per_pixel <= 8)
	var->bits_per_pixel = 8;
    else if (var->bits_per_pixel <= 16)
	var->bits_per_pixel = 16;
#if 0
    /* fbcon doesn't support this (yet) */
    else if (var->bits_per_pixel <= 24)
	var->bits_per_pixel = 24;
    else if (var->bits_per_pixel <= 32)
	var->bits_per_pixel = 32;
#endif
    else
	return -EINVAL;

    /*
     *  Memory limit
     */
    line_length = get_line_length(var->xres_virtual, var->bits_per_pixel);
    if (line_length*var->yres_virtual > videomemorysize)
	return -ENOMEM;

    set_color_bitfields(var);

    if ((activate & FB_ACTIVATE_MASK) == FB_ACTIVATE_NOW) {
	oldxres = display->var.xres;
	oldyres = display->var.yres;
	oldvxres = display->var.xres_virtual;
	oldvyres = display->var.yres_virtual;
	oldbpp = display->var.bits_per_pixel;
	display->var = *var;
	if (oldxres != var->xres || oldyres != var->yres ||
	    oldvxres != var->xres_virtual || oldvyres != var->yres_virtual ||
	    oldbpp != var->bits_per_pixel) {
	    struct fb_fix_screeninfo fix;

	    mc68x328fb_encode_fix(&fix, var);
	    display->screen_base = (char *)videomemory;
	    display->visual = fix.visual;
	    display->type = fix.type;
	    display->type_aux = fix.type_aux;
	    display->ypanstep = fix.ypanstep;
	    display->ywrapstep = fix.ywrapstep;
	    display->line_length = fix.line_length;
	    display->can_soft_blank = 1;
	    display->inverse = 0;
	    switch (var->bits_per_pixel) {
#ifdef FBCON_HAS_MFB
		case 1:
		    display->dispsw = &fbcon_mfb;
		    break;
#endif
#ifdef FBCON_HAS_CFB2
		case 2:
		    display->dispsw = &fbcon_cfb2;
		    break;
#endif
#ifdef FBCON_HAS_CFB4
		case 4:
		    display->dispsw = &fbcon_cfb4;
		    break;
#endif
#ifdef FBCON_HAS_CFB8
		case 8:
		    display->dispsw = &fbcon_cfb8;
		    break;
#endif
#ifdef FBCON_HAS_CFB16
		case 16:
		    display->dispsw = &fbcon_cfb16;
		    display->dispsw_data = fbcon_cmap.cfb16;
		    break;
#endif
#ifdef FBCON_HAS_CFB24
		case 24:
		    display->dispsw = &fbcon_cfb24;
		    display->dispsw_data = fbcon_cmap.cfb24;
		    break;
#endif
#ifdef FBCON_HAS_CFB32
		case 32:
		    display->dispsw = &fbcon_cfb32;
		    display->dispsw_data = fbcon_cmap.cfb32;
		    break;
#endif
		default:
		    display->dispsw = &fbcon_dummy;
		    break;
	    }
	    if (fb_info.changevar)
		(*fb_info.changevar)(con);
	}
	if (oldbpp != var->bits_per_pixel) {
	    if ((err = fb_alloc_cmap(&display->cmap, 0, 0)))
		return err;
	    do_install_cmap(con, info);
	}
    }
    return 0;
}


    /*
     *  Pan or Wrap the Display
     *
     *  This call looks only at xoffset, yoffset and the FB_VMODE_YWRAP flag
     */

static int mc68x328fb_pan_display(struct fb_var_screeninfo *var, int con,
			   struct fb_info *info)
{
    if (var->vmode & FB_VMODE_YWRAP) {
	if (var->yoffset < 0 ||
	    var->yoffset >= fb_display[con].var.yres_virtual ||
	    var->xoffset)
	    return -EINVAL;
    } else {
	if (var->xoffset+fb_display[con].var.xres >
	    fb_display[con].var.xres_virtual ||
	    var->yoffset+fb_display[con].var.yres >
	    fb_display[con].var.yres_virtual)
	    return -EINVAL;
    }
    fb_display[con].var.xoffset = var->xoffset;
    fb_display[con].var.yoffset = var->yoffset;
    if (var->vmode & FB_VMODE_YWRAP)
	fb_display[con].var.vmode |= FB_VMODE_YWRAP;
    else
	fb_display[con].var.vmode &= ~FB_VMODE_YWRAP;
    return 0;
}

    /*
     *  Get the Colormap
     */

static int mc68x328fb_get_cmap(struct fb_cmap *cmap, int kspc, int con,
			struct fb_info *info)
{
    if (con == currcon) /* current console? */
	return fb_get_cmap(cmap, kspc, mc68x328fb_getcolreg, info);
    else if (fb_display[con].cmap.len) /* non default colormap? */
	fb_copy_cmap(&fb_display[con].cmap, cmap, kspc ? 0 : 2);
    else
	fb_copy_cmap(fb_default_cmap(1<<fb_display[con].var.bits_per_pixel),
		     cmap, kspc ? 0 : 2);
    return 0;
}

    /*
     *  Set the Colormap
     */

static int mc68x328fb_set_cmap(struct fb_cmap *cmap, int kspc, int con,
			struct fb_info *info)
{
    int err;

    if (!fb_display[con].cmap.len) {	/* no colormap allocated? */
	if ((err = fb_alloc_cmap(&fb_display[con].cmap,
			      1<<fb_display[con].var.bits_per_pixel, 0)))
	    return err;
    }
    if (con == currcon)			/* current console? */
	return fb_set_cmap(cmap, kspc, mc68x328fb_setcolreg, info);
    else
	fb_copy_cmap(cmap, &fb_display[con].cmap, kspc ? 0 : 1);
    return 0;
}


int __init mc68x328fb_setup(char *options)
{
    char *this_opt;

    fb_info.fontname[0] = '\0';

    if (!options || !*options)
	return 0;

    while ((this_opt = strsep(&options, ",")) != NULL) {
	if (!strncmp(this_opt, "font:", 5))
	    strcpy(fb_info.fontname, this_opt+5);
    }
    return 0;
}


    /*
     *  Initialisation
     */

int __init mc68x328fb_init(void)
{
	/*
	 *  initialize the default mode from the LCD controller registers
	 */
	mc68x328fb_default.xres = LXMAX;
	mc68x328fb_default.yres = LYMAX+1;
	mc68x328fb_default.xres_virtual = mc68x328fb_default.xres;
	mc68x328fb_default.yres_virtual = mc68x328fb_default.yres;
	mc68x328fb_default.bits_per_pixel = 1 + (LPICF & 0x01);
	videomemory = LSSA;
	videomemorysize = (mc68x328fb_default.xres+7) / 8 *
		mc68x328fb_default.yres * mc68x328fb_default.bits_per_pixel;

    strcpy(fb_info.modename, mc68x328fb_name);
    fb_info.changevar = NULL;
    fb_info.node = -1;
    fb_info.fbops = &mc68x328fb_ops;
    fb_info.disp = &disp;
    fb_info.switch_con = &vfbcon_switch;
    fb_info.updatevar = &vfbcon_updatevar;
    fb_info.blank = &vfbcon_blank;
    fb_info.flags = FBINFO_FLAG_DEFAULT;

    mc68x328fb_set_var(&mc68x328fb_default, -1, &fb_info);

    if (register_framebuffer(&fb_info) < 0) {
	return -EINVAL;
    }

    printk(KERN_INFO "fb%d: %s frame buffer device\n", GET_FB_IDX(fb_info.node),
		mc68x328fb_name);
	printk(KERN_INFO "fb%d: %dx%dx%d at 0x%08lx\n", GET_FB_IDX(fb_info.node),
		mc68x328fb_default.xres, mc68x328fb_default.yres,
		1 << mc68x328fb_default.bits_per_pixel, videomemory);
    return 0;
}


static int vfbcon_switch(int con, struct fb_info *info)
{
    /* Do we have to save the colormap? */
    if (fb_display[currcon].cmap.len)
	fb_get_cmap(&fb_display[currcon].cmap, 1, mc68x328fb_getcolreg, info);

    currcon = con;
    /* Install new colormap */
    do_install_cmap(con, info);
    return 0;
}

    /*
     *  Update the `var' structure (called by fbcon.c)
     */

static int vfbcon_updatevar(int con, struct fb_info *info)
{
    /* Nothing */
    return 0;
}

    /*
     *  Blank the display.
     */

static void vfbcon_blank(int blank, struct fb_info *info)
{
    /* Nothing */
}

static u_long get_line_length(int xres_virtual, int bpp)
{
    u_long length;
    
    length = xres_virtual*bpp;
    length = (length+31)&-32;
    length >>= 3;
    return(length);
}

static void mc68x328fb_encode_fix(struct fb_fix_screeninfo *fix,
			   struct fb_var_screeninfo *var)
{
    memset(fix, 0, sizeof(struct fb_fix_screeninfo));
    strcpy(fix->id, mc68x328fb_name);
    fix->smem_start = videomemory;
    fix->smem_len = videomemorysize;
    fix->type = FB_TYPE_PACKED_PIXELS;
    fix->type_aux = 0;
    switch (var->bits_per_pixel) {
	case 1:
	    fix->visual = MC68X328FB_MONO_VISUAL;
	    break;
	case 2:
	case 4:
	case 8:
	    fix->visual = FB_VISUAL_PSEUDOCOLOR;
	    break;
	case 16:
	case 24:
	case 32:
	    fix->visual = FB_VISUAL_TRUECOLOR;
	    break;
    }
    fix->ywrapstep = 1;
    fix->xpanstep = 1;
    fix->ypanstep = 1;
    fix->line_length = get_line_length(var->xres_virtual, var->bits_per_pixel);
}

static void set_color_bitfields(struct fb_var_screeninfo *var)
{
    switch (var->bits_per_pixel) {
	case 1:
	case 8:
	    var->red.offset = 0;
	    var->red.length = 8;
	    var->green.offset = 0;
	    var->green.length = 8;
	    var->blue.offset = 0;
	    var->blue.length = 8;
	    var->transp.offset = 0;
	    var->transp.length = 0;
	    break;
	case 16:	/* RGB 565 */
	    var->red.offset = 0;
	    var->red.length = 5;
	    var->green.offset = 5;
	    var->green.length = 6;
	    var->blue.offset = 11;
	    var->blue.length = 5;
	    var->transp.offset = 0;
	    var->transp.length = 0;
	    break;
	case 24:	/* RGB 888 */
	    var->red.offset = 0;
	    var->red.length = 8;
	    var->green.offset = 8;
	    var->green.length = 8;
	    var->blue.offset = 16;
	    var->blue.length = 8;
	    var->transp.offset = 0;
	    var->transp.length = 0;
	    break;
	case 32:	/* RGBA 8888 */
	    var->red.offset = 0;
	    var->red.length = 8;
	    var->green.offset = 8;
	    var->green.length = 8;
	    var->blue.offset = 16;
	    var->blue.length = 8;
	    var->transp.offset = 24;
	    var->transp.length = 8;
	    break;
    }
    var->red.msb_right = 0;
    var->green.msb_right = 0;
    var->blue.msb_right = 0;
    var->transp.msb_right = 0;
}


    /*
     *  Read a single color register and split it into
     *  colors/transparent. Return != 0 for invalid regno.
     */

static int mc68x328fb_getcolreg(u_int regno, u_int *red, u_int *green, u_int *blue,
                         u_int *transp, struct fb_info *info)
{
    if (regno > 255)
	return 1;
    *red = (palette[regno].red<<8) | palette[regno].red;
    *green = (palette[regno].green<<8) | palette[regno].green;
    *blue = (palette[regno].blue<<8) | palette[regno].blue;
    *transp = 0;
    return 0;
}


    /*
     *  Set a single color register. The values supplied are already
     *  rounded down to the hardware's capabilities (according to the
     *  entries in the var structure). Return != 0 for invalid regno.
     */

static int mc68x328fb_setcolreg(u_int regno, u_int red, u_int green, u_int blue,
                         u_int transp, struct fb_info *info)
{
    if (regno > 255)
	return 1;
    red >>= 8;
    green >>= 8;
    blue >>= 8;
    palette[regno].red = red;
    palette[regno].green = green;
    palette[regno].blue = blue;
    return 0;
}


static void do_install_cmap(int con, struct fb_info *info)
{
    if (con != currcon)
	return;
    if (fb_display[con].cmap.len)
	fb_set_cmap(&fb_display[con].cmap, 1, mc68x328fb_setcolreg, info);
    else
	fb_set_cmap(fb_default_cmap(1<<fb_display[con].var.bits_per_pixel), 1,
		    mc68x328fb_setcolreg, info);
}


#ifdef MODULE
MODULE_LICENSE("GPL");

int init_module(void)
{
    return mc68x328fb_init();
}

void cleanup_module(void)
{
    unregister_framebuffer(&fb_info);
}

#endif /* MODULE */
