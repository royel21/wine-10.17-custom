/*
 * CD requests answered from the disc image loaded in the Winlator file manager
 */

#ifndef __WINE_MOUNTMGR_IMAGE_CD_H
#define __WINE_MOUNTMGR_IMAGE_CD_H

/* returns FALSE when no image is loaded or the request is not one answered from an image */
extern BOOL image_cd_ioctl( ULONG code, const void *in, ULONG in_len, void *out, ULONG out_len,
                            NTSTATUS *status, ULONG_PTR *info );

#endif
