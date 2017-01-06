/****************************************************************************
linux/arch/arm/mach-tcc892x/include/mach/tcc_ion.h
Description: TCC ION device driver

Copyright (C) 2013 Telechips Inc.

This program is free software; you can redistribute it and/or modify it under the terms
of the GNU General Public License as published by the Free Software Foundation;
either version 2 of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple Place,
Suite 330, Boston, MA 02111-1307 USA
****************************************************************************/

#ifndef __MACH_TCC_ION_H_

struct platform_device;

#if defined(CONFIG_ION_TCC)
extern struct platform_device tcc_ion_device;
void tcc_ion_set_platdata(void);
#else
static inline void tcc_ion_set_platdata(void)
{
}
#endif

#endif /* __MACH_S5PV310_ION_H_ */

