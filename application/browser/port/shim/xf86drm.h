/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	xf86drm.h
 *	The calls of the DRM user library that ANGLE's OpenGL ES backend
 *	makes. They are answered by the DRM shim of TessronOS's Mesa
 *	(lib/libgl/gl_drm.c of TessronOS, linked with make GL=1), whose
 *	drmVersion has this layout. There is no device file to open, so
 *	ANGLE never gets a descriptor to ask about.
 */

#ifndef BR_SHIM_XF86DRM_H
#define BR_SHIM_XF86DRM_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _drmVersion {
	int	version_major;
	int	version_minor;
	int	version_patchlevel;
	int	name_len;
	char	*name;
	int	date_len;
	char	*date;
	int	desc_len;
	char	*desc;
} drmVersion, *drmVersionPtr;

drmVersionPtr	drmGetVersion( int fd );
void		drmFreeVersion( drmVersionPtr v );

#ifdef __cplusplus
}
#endif

#endif /* BR_SHIM_XF86DRM_H */
