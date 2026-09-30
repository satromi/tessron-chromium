/*
 *----------------------------------------------------------------------
 *    TessronOS
 *    Copyright (C) 2026 satromi
 *
 *    This software is distributed under the MIT License (LICENSE).
 *----------------------------------------------------------------------
 */

/*
 *	linux/magic.h
 *	The file system type numbers statfs reports on Linux. TessronOS has
 *	none of these file systems; the numbers are there for the sources
 *	that compare with them.
 */

#ifndef __TS_LINUX_MAGIC_H__
#define __TS_LINUX_MAGIC_H__

#define TMPFS_MAGIC		0x01021994
#define PROC_SUPER_MAGIC	0x9fa0
#define SYSFS_MAGIC		0x62656572
#define CGROUP_SUPER_MAGIC	0x27e0eb
#define CGROUP2_SUPER_MAGIC	0x63677270
#define NFS_SUPER_MAGIC		0x6969
#define SMB_SUPER_MAGIC		0x517B
#define CIFS_SUPER_MAGIC	0xFF534D42
#define HUGETLBFS_MAGIC		0x958458f6
#define RAMFS_MAGIC		0x858458f6
#define EXT4_SUPER_MAGIC	0xEF53
#define MSDOS_SUPER_MAGIC	0x4d44

#endif /* __TS_LINUX_MAGIC_H__ */
