/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sysutils/NetlinkEvent.h>
#include <sys/param.h>

#include <linux/kdev_t.h>

#include <cutils/properties.h>

#include <diskconfig/diskconfig.h>

#include <private/android_filesystem_config.h>

#define LOG_TAG "Vold"

#include <cutils/log.h>

#include "Loop.h"
#include "Volume.h"
#include "VolumeManager.h"
#include "ResponseCode.h"
#include "Fat.h"
#include "Ntfs.h"
#include "Exfat.h"
#include "Hfsplus.h"
#ifdef HAS_ISO9660
#include "Iso9660.h"
#endif
#include "Process.h"
#include "cryptfs.h"

extern "C" void dos_partition_dec(void const *pp, struct dos_partition *d);
extern "C" void dos_partition_enc(void *pp, struct dos_partition *d);

#ifdef HAS_VIRTUAL_CDROM
#define LOOP_DEV "/dev/block/loop0"

bool Volume::sLoopMounted=false;
char * Volume::mloopmapdir=NULL;
char * Volume::mloopmountdir=NULL;
#endif

bool Volume::sFakeSdcard = false;
bool Volume::sSdcardMounted = false;
bool Volume::sVirtualSdcard = false;
bool Volume::sVirtualSdcardMounted = false;
bool Volume::sFlashMouted=false;

const char* MSGFMT_FAKE_ADD_SDCARD =
        "add@/devices/amlogic/fakesdcard " \
        "ACTION=add " \
        "DEVPATH=/devices/amlogic/fakesdcard " \
        "SUBSYSTEM=block " \
        "MAJOR=%d MINOR=%d " \
        "DEVNAME=sdcard " \
        "DEVTYPE=disk NPARTS=0 SEQNUM=999";

const char* MSGFMT_FAKE_REMOVE_SDCARD =
        "remove@/devices/amlogic/fakesdcard " \
        "ACTION=remove " \
        "DEVPATH=/devices/amlogic/fakesdcard " \
        "SUBSYSTEM=block " \
        "MAJOR=%d MINOR=%d " \
        "DEVNAME=sdcard " \
        "DEVTYPE=disk NPARTS=0 SEQNUM=999";

/*
 * Secure directory - stuff that only root can see
 */
const char *Volume::SECDIR            = "/mnt/secure";

/*
 * Secure staging directory - where media is mounted for preparation
 */
const char *Volume::SEC_STGDIR        = "/mnt/secure/staging";

/*
 * Path to the directory on the media which contains publicly accessable
 * asec imagefiles. This path will be obscured before the mount is
 * exposed to non priviledged users.
 */
const char *Volume::SEC_STG_SECIMGDIR = "/mnt/secure/staging/.android_secure";

/*
 * Path to where *only* root can access asec imagefiles
 */
const char *Volume::SEC_ASECDIR       = "/mnt/secure/asec";

/*
 * Path to where secure containers are mounted
 */
const char *Volume::ASECDIR           = "/mnt/asec";

/*
 * Path to where OBBs are mounted
 */
const char *Volume::LOOPDIR           = "/mnt/obb";

static const char *stateToStr(int state) {
    if (state == Volume::State_Init)
        return "Initializing";
    else if (state == Volume::State_NoMedia)
        return "No-Media";
    else if (state == Volume::State_Idle)
        return "Idle-Unmounted";
    else if (state == Volume::State_Pending)
        return "Pending";
    else if (state == Volume::State_Mounted)
        return "Mounted";
    else if (state == Volume::State_Unmounting)
        return "Unmounting";
    else if (state == Volume::State_Checking)
        return "Checking";
    else if (state == Volume::State_Formatting)
        return "Formatting";
    else if (state == Volume::State_Shared)
        return "Shared-Unmounted";
    else if (state == Volume::State_SharedMnt)
        return "Shared-Mounted";
    else if (state == Volume::State_Deleting)
        return "Deleting";
    else
        return "Unknown-Error";
}

Volume::Volume(VolumeManager *vm, const char *label, const char *mount_point) {
    mVm = vm;
    mDebug = false;
    mLabel = strdup(label);
    mMountpoint = strdup(mount_point);
    mState = Volume::State_Init;
    mPartIdx = -1;
    mMountedPartMap = 0;
    mValidPartMap = 0;
    mVolumeType = VOLUME_TYPE_UNKNOWN;
    mSdcardPartitionBit = 0;
    mFakeSdcardLink[0] = 0;
    mHasAsec = false;
}

Volume::~Volume() {
    free(mLabel);
    free(mMountpoint);
}

void Volume::protectFromAutorunStupidity() {
    char filename[255];

    snprintf(filename, sizeof(filename), "%s/autorun.inf", SEC_STGDIR);
    if (!access(filename, F_OK)) {
        SLOGW("Volume contains an autorun.inf! - removing");
        /*
         * Ensure the filename is all lower-case so
         * the process killer can find the inode.
         * Probably being paranoid here but meh.
         */
        rename(filename, filename);
        Process::killProcessesWithOpenFiles(filename, 2);
        if (unlink(filename)) {
            SLOGE("Failed to remove %s (%s)", filename, strerror(errno));
        }
    }
}

void Volume::setDebug(bool enable) {
    mDebug = enable;
}

dev_t Volume::getDiskDevice() {
    return MKDEV(0, 0);
};

dev_t Volume::getShareDevice() {
    return getDiskDevice();
}

void Volume::handleVolumeShared() {
}

void Volume::handleVolumeUnshared() {
}

int Volume::handleBlockEvent(NetlinkEvent *evt) {
    errno = ENOSYS;
    return -1;
}

void Volume::setState(int state) {
    char msg[255];
    int oldState = mState;

    if (oldState == state) {
        SLOGW("Duplicate state (%d)\n", state);
        return;
    }

    if (oldState == State_Deleting) {
        SLOGW("Volume::setState(%d %s) oldState is State_Deleting. volume may have been deleted",
                state, stateToStr(state));
        return;
    }

    mState = state;

    SLOGD("Volume %s state changing %d (%s) -> %d (%s)", mLabel,
         oldState, stateToStr(oldState), mState, stateToStr(mState));
    if (state != State_Deleting) {
    snprintf(msg, sizeof(msg),
             "Volume %s %s state changed from %d (%s) to %d (%s)", getLabel(),
             getMountpoint(), oldState, stateToStr(oldState), mState,
             stateToStr(mState));

    mVm->getBroadcaster()->sendBroadcast(ResponseCode::VolumeStateChange,
                                         msg, false);
    }
}

void Volume::setVirtualSdcardState(int oldState, int newState) {
    char msg[255];

    SLOGD("VSDCARD: Volume %s state changing %d (%s) -> %d (%s)", "sdcard",
         oldState, stateToStr(oldState), newState, stateToStr(newState));
    
    snprintf(msg, sizeof(msg),
             "Volume %s %s state changed from %d (%s) to %d (%s)", "sdcard",
             "/mnt/sdcard", oldState, stateToStr(oldState), newState,
             stateToStr(newState));

    mVm->getBroadcaster()->sendBroadcast(ResponseCode::VolumeStateChange,
                                         msg, false);
    
}

int Volume::createDeviceNode(const char *path, int major, int minor) {
    mode_t mode = 0660 | S_IFBLK;
    dev_t dev = (major << 8) | minor;
    if (mknod(path, mode, dev) < 0) {
        if (errno != EEXIST) {
            return -1;
        }
    }
    return 0;
}

int Volume::formatVol() {

    if (getState() == Volume::State_NoMedia) {
        errno = ENODEV;
        return -1;
    } else if (getState() != Volume::State_Idle) {
        errno = EBUSY;
        return -1;
    }

    if (isMountpointMounted(getMountpoint())) {
        SLOGW("Volume is idle but appears to be mounted - fixing");
        setState(Volume::State_Mounted);
        // mCurrentlyMountedKdev = XXX
        errno = EBUSY;
        return -1;
    }

    bool formatEntireDevice = (mPartIdx == -1);
    char devicePath[255];
    char *label = NULL;
    dev_t diskNode = getDiskDevice();
    dev_t partNode = MKDEV(MAJOR(diskNode), (formatEntireDevice ? MINOR(diskNode) + 1 : getVolumeMinor(mPartIdx-1)));

    setState(Volume::State_Formatting);

    if (mDebug)
        SLOGI("Formatting volume %s mpartIdx=%d partNode=%x", getLabel(), mPartIdx, partNode);

    // Only initialize the MBR if we are formatting the entire device
    if (formatEntireDevice) {
        sprintf(devicePath, "/dev/block/vold/%d:%d",
                MAJOR(diskNode), MINOR(diskNode));

        if (initializeMbr(devicePath)) {
            SLOGE("Failed to initialize MBR (%s)", strerror(errno));
            goto err;
        }
    }

    sprintf(devicePath, "/dev/block/vold/%d:%d",
            MAJOR(partNode), MINOR(partNode));

    if (mDebug) {
        SLOGI("Formatting volume %s major=%d minor=%d", getLabel(), MAJOR(diskNode), MINOR(diskNode));
        SLOGI("Formatting volume %s (%s)", getLabel(), devicePath);
    }

#ifdef RECOVERY_MEDIA_LABEL
    if (VOLUME_TYPE_FLASH == mVolumeType)
        label = RECOVERY_MEDIA_LABEL;
#endif
    if (Fat::format(devicePath, 0, label)) {
        SLOGE("Failed to format (%s)", strerror(errno));
        goto err;
    }

    setState(Volume::State_Idle);
    return 0;
err:
    return -1;
}

bool Volume::isMountpointMounted(const char *path) {
    char device[256];
    char mount_path[256];
    char rest[256];
    FILE *fp;
    char line[1024];

    if (!(fp = fopen("/proc/mounts", "r"))) {
        SLOGE("Error opening /proc/mounts (%s)", strerror(errno));
        return false;
    }

    while(fgets(line, sizeof(line), fp)) {
        line[strlen(line)-1] = '\0';
        sscanf(line, "%255s %255s %255s\n", device, mount_path, rest);
        if (!strcmp(mount_path, path)) {
            fclose(fp);
            return true;
        }

    }

    fclose(fp);
    return false;
}

int Volume::doFsCheck(const char *devicePath) {
#ifdef HAS_EXFAT
    if (Exfat::check(devicePath)) {
        if (errno == ENODATA)
            SLOGW("%s does not contain a exFAT filesystem\n", devicePath);
	 else if(errno == EIO)
	 	return 0;		//in this case,volume is exfat,but fsck find error,don't need msdos fsck.
#endif
    if (Fat::check(devicePath)) {
        if (errno == ENODATA)
            SLOGW("%s does not contain a FAT filesystem\n", devicePath);
        if (Hfsplus::check(devicePath)) {
            if (errno == ENODATA)
                SLOGW("%s does not contain an HFS+ filesystem\n", devicePath);
            if (Ntfs::check(devicePath)) {
                if (errno == ENODATA) {
                    SLOGW("%s does not contain an NTFS filesystem\n", devicePath);
                    return ENODATA;
                }
                errno = EIO;
                /* Badness - abort the mount */
                SLOGE("%s failed FS checks (%s)", devicePath, strerror(errno));
                return EIO;
            }
        }
    }
#ifdef HAS_EXFAT
    }
#endif
    return 0;
}

int Volume::doMount(const char *devicePath, const char *mountpoint) {
	char* p = strrchr(mountpoint, '/');
	char mvl[64];
	sprintf(mvl, "volume.label.%s", p+1);

#ifdef HAS_EXFAT
	if (Exfat::doMount(devicePath, mountpoint, false,false,1000, 1015, 0002, true)) {
		SLOGE("%s failed to mount via exFAT (%s). Trying VFAT...",
			devicePath, strerror(errno));
#endif
		if (Fat::doMount(devicePath, mountpoint, false, false, false, 1000, 1015, 0002, true)) {
			SLOGE("%s failed to mount via VFAT (%s). Trying NTFS...",
				devicePath, strerror(errno));
			if (Ntfs::doMount(devicePath, mountpoint, false, false, 1000, 1015, 0002, true)) {
				SLOGE("%s failed to mount via NTFS (%s). Trying HFS+...",
					devicePath, strerror(errno));
				if (Hfsplus::doMount(devicePath, mountpoint, false, false, 1000, 1015, 0002, true)) {
#ifdef HAS_ISO9660	
			 		if(VOLUME_TYPE_FLASH == mVolumeType)
			 		{
							SLOGE("%s failed to mount via HFS+ (%s). Device mount failed.",
                                				devicePath, strerror(errno));	
                          				return -1;
			 		}
			 		else
			 		{
                    				SLOGE("%s failed to mount via HFS+ (%s). Trying iso9660...",
                           				devicePath, strerror(errno));
			  			if (iso9660::doMount(devicePath, mountpoint, false, false, 1000, 1015, 0002, true)) {
                          				SLOGE("%s failed to mount via iso9660 (%s). Device mount failed.",
                                				devicePath, strerror(errno));	
                          				return -1;
		      	     			}else{
							property_set(mvl, "ISO9660");
			       		}
			 		}
#else
			 		SLOGE("%s failed to mount via NTFS (%s). Device mount failed.",
                                		devicePath, strerror(errno));	
                  			return -1;
#endif
				}
				else {
                            	property_set(mvl, "HFSPLUS");
                       	}
			}
			else {
				property_set(mvl, "NTFS");
			}
		}
#ifdef HAS_EXFAT
	}
#endif
	
	return 0;
}

int Volume::unmountbeforepart(int curidx)
{
	int i;
	char mount_pointer[255] = {0};
	char mount_dir[32]={0};
	const char *label = NULL;
	
        SLOGD("-------------DISK IS REMOVED BEFORE MOUNTED OK\n");      
        for(i=0;i<curidx;i++)
        {
              if (VOLUME_TYPE_UMS == mVolumeType || VOLUME_TYPE_SATA == mVolumeType) {
#ifdef FUNCTION_UMS_PARTITION
	                sprintf(mount_pointer, "%s", getMountpoint());
#else
                    if (mNoParts) {
                        label = getLabel();
                    } else if (((1 << i) & mValidPartMap) == 0) {
                         continue;
                    } else {
                          label = getDeviceNodesLabel(i);
                          if (NULL == label)
                              continue;
                     }
			         sprintf(mount_dir, "%s", getMountpoint());			
                    sprintf(mount_pointer, "%s/%s", getMountpoint(), label);
#endif
               }
				 else {
                   sprintf(mount_pointer, "%s", getMountpoint());
               }

				 doUnmount(mount_pointer,true);
          }  	
     
	 return 0;
}

#ifndef 	FUNCTION_UMS_PARTITION		
int Volume::unmountdisk(char * path)
{
		DIR *pDevDir = NULL ;
       struct dirent *dirp = NULL;
       char mount_pointer[255] = {0};
			 
		SLOGW("Volume is deleted,but mounted ok,we must fix it by file operation\n");

		pDevDir = opendir(path);
		if(pDevDir == NULL){
            LOGE("failed to opendir(%s),err:%s\n",path,strerror(errno));
            return -1 ;
        }
    
        while((dirp=readdir(pDevDir)) != NULL){
				if (!strcmp(dirp->d_name, ".") || !strcmp(dirp->d_name, ".."))
					continue;
        		sprintf(mount_pointer, "%s/%s", path, dirp->d_name);
				doUnmount(mount_pointer,true);

				rmdir(mount_pointer);
        }
        closedir(pDevDir);

		 return 0;
}
#endif

int Volume::mountVol() {
    dev_t deviceNodes[MAX_PARTS];
    int n, i, rc = 0;
    char errmsg[255];
#ifndef 	FUNCTION_UMS_PARTITION			
	char mount_dir[32]={0};
#endif
    const char* externalStorage = getenv("EXTERNAL_STORAGE");
    bool primaryStorage = externalStorage && !strcmp(getMountpoint(), externalStorage);
    char decrypt_state[PROPERTY_VALUE_MAX];
    char crypto_state[PROPERTY_VALUE_MAX];
    char encrypt_progress[PROPERTY_VALUE_MAX];
    int flags;

    property_get("vold.decrypt", decrypt_state, "");
    property_get("vold.encrypt_progress", encrypt_progress, "");

    /* Don't try to mount the volumes if we have not yet entered the disk password
     * or are in the process of encrypting.
     */
    if (getState() != Volume::State_Idle) {
 	     SLOGE("Volume::State is not Idel\n");
        errno = EBUSY;
        return -1;
    }

    if (isMountpointMounted(getMountpoint())) {
        SLOGW("Volume is idle but appears to be mounted - fixing");
        setState(Volume::State_Mounted);
        // mCurrentlyMountedKdev = XXX
        return 0;
    }

    n = getDeviceNodes((dev_t *) &deviceNodes, MAX_PARTS);
    if (!n) {
        SLOGE("Failed to get device nodes (%s)\n", strerror(errno));
        return -1;
    }

    /* If we're running encrypted, and the volume is marked as encryptable and nonremovable,
     * and vold is asking to mount the primaryStorage device, then we need to decrypt
     * that partition, and update the volume object to point to it's new decrypted
     * block device
     */
    property_get("ro.crypto.state", crypto_state, "");
    flags = getFlags();
    if (primaryStorage &&
        ((flags & (VOL_NONREMOVABLE | VOL_ENCRYPTABLE))==(VOL_NONREMOVABLE | VOL_ENCRYPTABLE)) &&
        !strcmp(crypto_state, "encrypted") && !isDecrypted()) {
       char new_sys_path[MAXPATHLEN];
       char nodepath[256];
       int new_major, new_minor;

       if (n != 1) {
           /* We only expect one device node returned when mounting encryptable volumes */
           SLOGE("Too many device nodes returned when mounting %d\n", getMountpoint());
           return -1;
       }

       if (cryptfs_setup_volume(getLabel(), MAJOR(deviceNodes[0]), MINOR(deviceNodes[0]),
                                new_sys_path, sizeof(new_sys_path),
                                &new_major, &new_minor)) {
           SLOGE("Cannot setup encryption mapping for %d\n", getMountpoint());
           return -1;
       }
       /* We now have the new sysfs path for the decrypted block device, and the
        * majore and minor numbers for it.  So, create the device, update the
        * path to the new sysfs path, and continue.
        */
        snprintf(nodepath,
                 sizeof(nodepath), "/dev/block/vold/%d:%d",
                 new_major, new_minor);
        if (createDeviceNode(nodepath, new_major, new_minor)) {
            SLOGE("Error making device node '%s' (%s)", nodepath,
                                                       strerror(errno));
        }

        // Todo: Either create sys filename from nodepath, or pass in bogus path so
        //       vold ignores state changes on this internal device.
        updateDeviceInfo(nodepath, new_major, new_minor);

        /* Get the device nodes again, because they just changed */
        n = getDeviceNodes((dev_t *) &deviceNodes, 4);
        if (!n) {
            SLOGE("Failed to get device nodes (%s)\n", strerror(errno));
            return -1;
        }
    }

    for (i = 0; i < n; i++) {
        char devicePath[255];
        char mount_pointer[255] = {0};
        const char *label = NULL;
        int mount_index = 0;

        if (getState() == Volume::State_Deleting) {
#ifndef  FUNCTION_UMS_PARTITION
            if(mount_dir[0]!=0)
                  unmountdisk(mount_dir);
#endif
	     SLOGE("Volume::State is Deleting\n");		
            errno = ENODEV;
            return -1;
        } else if (getState() == Volume::State_NoMedia) {
            unmountbeforepart(i);       
            snprintf(errmsg, sizeof(errmsg),
                     "Volume %s %s mount failed - no media",
                     getLabel(), getMountpoint());
            mVm->getBroadcaster()->sendBroadcast(
                                             ResponseCode::VolumeMountFailedNoMedia,
                                             errmsg, false);
	     SLOGE("Volume::State is NoMedia\n");		
            errno = ENODEV;
            return -1;
        }

        if (VOLUME_TYPE_UMS == mVolumeType){
#ifdef FUNCTION_UMS_PARTITION
	     sprintf(mount_pointer, "%s", getMountpoint());
            mkdir(mount_pointer, 0755);	
#else
            if (mNoParts) {
                label = getLabel();
            } else if (((1 << i) & mValidPartMap) == 0) {
                continue;
            } else {
                label = getDeviceNodesLabel(i);
                if (NULL == label)
                    continue;
            }
            mkdir(getMountpoint(), 0755);
            sprintf(mount_pointer, "%s/%s", getMountpoint(), label);
            mkdir(mount_pointer, 0755);
#endif
        } 
		 else if( VOLUME_TYPE_SATA == mVolumeType)
        {
            if (mNoParts) {
                label = getLabel();
            } else if (((1 << i) & mValidPartMap) == 0) {
                continue;
            } else {
                label = getDeviceNodesLabel(i);
                if (NULL == label)
                    continue;
            }
            mkdir(getMountpoint(), 0755);
            sprintf(mount_pointer, "%s/%s", getMountpoint(), label);
            mkdir(mount_pointer, 0755);
        } else {
            sprintf(mount_pointer, "%s", getMountpoint());
            mkdir(mount_pointer, 0755);
        }

        sprintf(devicePath, "/dev/block/vold/%d:%d", MAJOR(deviceNodes[i]),
                MINOR(deviceNodes[i]));

        SLOGI("%s being considered for partition %s in volume %s at %s i=%d type=%d\n",
              devicePath, label ? label : "", getLabel(), mount_pointer, i, mVolumeType);

        errno = 0;
        setState(Volume::State_Checking);

        //TODO get fs type
        rc = doFsCheck(devicePath);
        if (ENODATA == rc)
                continue;
        else if (EIO == rc) {
            setState(Volume::State_Idle);
            return -1;
        }

        errno = 0;
        int gid;
        if (primaryStorage) {
            // Special case the primary SD card.
            // For this we grant write access to the SDCARD_RW group.
            gid = AID_SDCARD_RW;
        } else {
            // For secondary external storage we keep things locked up.
            gid = AID_MEDIA_RW;
        }

       if(mHasAsec) {
	     if(sVirtualSdcard && sVirtualSdcardMounted)
	     {
	     	unlink("/mnt/sdcard");

	       mkdir("/mnt/sdcard",0755);
	     }
        /*
         * Mount the device on our internal staging mountpoint so we can
         * muck with it before exposing it to non priviledged users.
         */
            if (doMount(devicePath, "/mnt/secure/staging"))
                continue;

            SLOGI("Device %s, target %s mounted @ /mnt/secure/staging", devicePath, getMountpoint());

            protectFromAutorunStupidity();

            // only create android_secure on primary storage
            if (primaryStorage && createBindMounts()) {
                SLOGE("Failed to create bindmounts (%s)", strerror(errno));
                umount("/mnt/secure/staging");
                setState(Volume::State_Idle);
                return -1;
            }

        /*
         * Now that the bindmount trickery is done, atomically move the
         * whole subtree to expose it to non priviledged users.
         */
            if (doMoveMount("/mnt/secure/staging", getMountpoint(), false)) {
                SLOGE("Failed to move mount (%s)", strerror(errno));
                umount("/mnt/secure/staging");
                setState(Volume::State_Idle);
                return -1;
            }
            sSdcardMounted = true;
        } else {
			char* p = strrchr(mount_pointer, '/');
            char mvl[64];
            sprintf(mvl, "volume.label.%s", p+1);
            if (doMount(devicePath, mount_pointer)) {
                rmdir(mount_pointer);
                continue;
            } else {
            	if (VOLUME_TYPE_FLASH == mVolumeType) {
			if(sVirtualSdcard)
			{
            			if (!sVirtualSdcardMounted&&!sSdcardMounted) {	
	            			char vsd_path[255];
					snprintf(vsd_path,
	                         		sizeof(vsd_path), "%s/.vsdcard",
	                         		mount_pointer);            		
	            			SLOGW("VSDCARD: symlink %s -> %s", "/mnt/sdcard", vsd_path);
	            			rmdir("/mnt/sdcard");
	            			mkdir(vsd_path, 0755);
	            			symlink(vsd_path, "/mnt/sdcard");
			      				
	            			setVirtualSdcardState(Volume::State_Idle, Volume::State_Mounted);
	            			sVirtualSdcardMounted = true;
            			}
			}
			sFlashMouted=1;		
            	}            	
            	
                if (sFakeSdcard && !sSdcardMounted && mSdcardPartitionBit == 0) {
                    if (!umount(mount_pointer) || errno == EINVAL || errno == ENOENT) {
                        rmdir(mount_pointer);
                        NetlinkEvent *evt = newFakeSdcardEvent(0,
                                MAJOR(deviceNodes[i]), MINOR(deviceNodes[i]));
                        mVm->handleBlockEvent(evt);
                        delete evt;
                        memcpy(mFakeSdcardLink, mount_pointer, sizeof(mFakeSdcardLink));
                        symlink("/mnt/sdcard", mFakeSdcardLink);
                        mSdcardPartitionBit = mNoParts ? (1 << 31) : (1 << i);
                    }
                }
				 property_set(mvl, "VFAT");
            }
        }

        if (mNoParts) {
            mMountedPartMap |= (1 << 31);
        } else {
            mMountedPartMap |= (1 << i);
        }
    }

    SLOGI("Volume::mount mounted partitions: 0x%x", mMountedPartMap);
    if (mMountedPartMap) {
        if ((getState() == Volume::State_Deleting) ||(getState() == Volume::State_NoMedia)){
				if(getState() == Volume::State_NoMedia)
			         unmountbeforepart(n);					  
#ifndef FUNCTION_UMS_PARTITION
             if((getState() == Volume::State_Deleting)&&(mount_dir[0]!=0))
			         unmountdisk(mount_dir);		 	
             if (VOLUME_TYPE_UMS == mVolumeType || VOLUME_TYPE_SATA == mVolumeType) {
		         SLOGE("1----rmdir %s\n", mount_dir);			
                rmdir(mount_dir);
             }			 
#endif
			   return -1;	
		  }
        setState(Volume::State_Mounted);
        return 0;
    } else {
#ifndef FUNCTION_UMS_PARTITION    
        if (VOLUME_TYPE_UMS == mVolumeType || VOLUME_TYPE_SATA == mVolumeType) {
		     SLOGE("2-----rmdir %s\n", mount_dir);			
            rmdir(mount_dir);
        }
#endif
        SLOGE("Volume %s found no suitable devices for mounting :(\n", getLabel());
        setState(Volume::State_Idle);
        return -1;
    }
}

int Volume::createBindMounts() {
    unsigned long flags;

    /*
     * Rename old /android_secure -> /.android_secure
     */
    if (!access("/mnt/secure/staging/android_secure", R_OK | X_OK) &&
         access(SEC_STG_SECIMGDIR, R_OK | X_OK)) {
        if (rename("/mnt/secure/staging/android_secure", SEC_STG_SECIMGDIR)) {
            SLOGE("Failed to rename legacy asec dir (%s)", strerror(errno));
        }
    }

    /*
     * Ensure that /android_secure exists and is a directory
     */
    if (access(SEC_STG_SECIMGDIR, R_OK | X_OK)) {
        if (errno == ENOENT) {
            if (mkdir(SEC_STG_SECIMGDIR, 0777)) {
                SLOGE("Failed to create %s (%s)", SEC_STG_SECIMGDIR, strerror(errno));
                return -1;
            }
        } else {
            SLOGE("Failed to access %s (%s)", SEC_STG_SECIMGDIR, strerror(errno));
            return -1;
        }
    } else {
        struct stat sbuf;

        if (stat(SEC_STG_SECIMGDIR, &sbuf)) {
            SLOGE("Failed to stat %s (%s)", SEC_STG_SECIMGDIR, strerror(errno));
            return -1;
        }
        if (!S_ISDIR(sbuf.st_mode)) {
            SLOGE("%s is not a directory", SEC_STG_SECIMGDIR);
            errno = ENOTDIR;
            return -1;
        }
    }

    /*
     * Bind mount /mnt/secure/staging/android_secure -> /mnt/secure/asec so we'll
     * have a root only accessable mountpoint for it.
     */
    if (mount(SEC_STG_SECIMGDIR, SEC_ASECDIR, "", MS_BIND, NULL)) {
        SLOGE("Failed to bind mount points %s -> %s (%s)",
                SEC_STG_SECIMGDIR, SEC_ASECDIR, strerror(errno));
        return -1;
    }

    /*
     * Mount a read-only, zero-sized tmpfs  on <mountpoint>/android_secure to
     * obscure the underlying directory from everybody - sneaky eh? ;)
     */
    if (mount("tmpfs", SEC_STG_SECIMGDIR, "tmpfs", MS_RDONLY, "size=0,mode=000,uid=0,gid=0")) {
        SLOGE("Failed to obscure %s (%s)", SEC_STG_SECIMGDIR, strerror(errno));
        umount("/mnt/asec_secure");
        return -1;
    }

    return 0;
}

int Volume::doMoveMount(const char *src, const char *dst, bool force) {
    unsigned int flags = MS_MOVE;
    int retries = 5;

    while(retries--) {
        if (!mount(src, dst, "", flags, NULL)) {
            if (mDebug) {
                SLOGD("Moved mount %s -> %s sucessfully", src, dst);
            }
            return 0;
        } else if (errno != EBUSY) {
            SLOGE("Failed to move mount %s -> %s (%s)", src, dst, strerror(errno));
            return -1;
        }
        int action = 0;

        if (force) {
            if (retries == 1) {
                action = 2; // SIGKILL
            } else if (retries == 2) {
                action = 1; // SIGHUP
            }
        }
        SLOGW("Failed to move %s -> %s (%s, retries %d, action %d)",
                src, dst, strerror(errno), retries, action);
        Process::killProcessesWithOpenFiles(src, action);
        usleep(1000*250);
    }

    errno = EBUSY;
    SLOGE("Giving up on move %s -> %s (%s)", src, dst, strerror(errno));
    return -1;
}

int Volume::doUnmount(const char *path, bool force) {
    int retries = 20;

    if (mDebug) {
        SLOGD("Unmounting {%s}, force = %d", path, force);
    }

    while (retries--) {
        if (!umount(path) || errno == EINVAL || errno == ENOENT) {
            SLOGI("%s sucessfully unmounted", path);
            return 0;
        }

        int action = 0;

        if (force) {
            if (retries == 11) {
                action = 2; // SIGKILL
            } else if (retries == 12) {
                action = 1; // SIGHUP
            }
        }

        SLOGW("Failed to unmount %s (%s, retries %d, action %d)",
                path, strerror(errno), retries, action);

        Process::killProcessesWithOpenFiles(path, action);
        usleep(1000*1000);
    }
    errno = EBUSY;
    SLOGE("Giving up on unmount %s (%s)", path, strerror(errno));
    return -1;
}

int Volume::unmountVol(bool force, bool revert) {
    int i, rc;

#ifdef HAS_VIRTUAL_CDROM
	if(sLoopMounted==true)
	{
		char * mountpoint;
       mountpoint = (char *)getMountpoint();
		if(strncmp(mountpoint,mloopmapdir,strlen(mountpoint))==0)
		{
			unmountloop(true);     
	 	}
    }
#endif
    if (getState() != Volume::State_Mounted) {
        if (!isMountpointMounted(getMountpoint())) {
        SLOGE("Volume %s unmount request when not mounted", getLabel());
        errno = EINVAL;
            return -1;
        } else
            SLOGE("Volume %s unmount request when mounted, but not State_Mounted. Trying anyways", getLabel());
    }

    setState(Volume::State_Unmounting);
    usleep(1000 * 1000); // Give the framework some time to react

    if (sFakeSdcard && sSdcardMounted && mSdcardPartitionBit != 0) {
        if (unmountFakeSdcard()) {
            mMountedPartMap &= ~mSdcardPartitionBit;
            mSdcardPartitionBit = 0;
            sSdcardMounted = false;
        }
    }

    while (mMountedPartMap) {
        int part_index = 0;

        // Get partition to be unmounted
        if (mMountedPartMap & (1 << 31)) {
            SLOGD("Volume::unmountVol partitions=0x%x type=%u", mMountedPartMap, mVolumeType);
        } else {
            for (part_index = 0; part_index < MAX_PARTS; part_index++) {
                SLOGD("Volume::unmountVol partitions=0x%x type=%u index=%u",
                      mMountedPartMap, mVolumeType, part_index);
                if (mMountedPartMap & (1 << part_index))
                    break;
            }
        }

        if (mHasAsec) {
    /*
     * First move the mountpoint back to our internal staging point
     * so nobody else can muck with it while we work.
     */
    if (doMoveMount(getMountpoint(), SEC_STGDIR, force)) {
        SLOGE("Failed to move mount %s => %s (%s)", getMountpoint(), SEC_STGDIR, strerror(errno));
        setState(Volume::State_Mounted);
        return -1;
    }

    protectFromAutorunStupidity();

    /*
     * Unmount the tmpfs which was obscuring the asec image directory
     * from non root users
     */

    if (doUnmount(Volume::SEC_STG_SECIMGDIR, force)) {
        SLOGE("Failed to unmount tmpfs on %s (%s)", SEC_STG_SECIMGDIR, strerror(errno));
        goto fail_republish;
    }

    /*
     * Remove the bindmount we were using to keep a reference to
     * the previously obscured directory.
     */

    if (doUnmount(Volume::SEC_ASECDIR, force)) {
        SLOGE("Failed to remove bindmount on %s (%s)", SEC_ASECDIR, strerror(errno));
        goto fail_remount_tmpfs;
    }

    /*
     * Finally, unmount the actual block device from the staging dir
     */
    if (doUnmount(Volume::SEC_STGDIR, force)) {
        SLOGE("Failed to unmount %s (%s)", SEC_STGDIR, strerror(errno));
        goto fail_recreate_bindmount;
    }

            if (mMountedPartMap & (1 << 31))
                mMountedPartMap &= ~(1 << 31);
            else
                mMountedPartMap &= ~(1 << part_index);
            sSdcardMounted = false;
        } else if (VOLUME_TYPE_FLASH == mVolumeType || VOLUME_TYPE_SDCARD == mVolumeType) {
            // Only unmount one partition from flash
            if (doUnmount(getMountpoint(), true)) {
                SLOGE("Failed to unmount %s (%s)", getMountpoint(), strerror(errno));
                return -1;
            }
            mMountedPartMap = 0;
	     sFlashMouted = 0;		
        } else {
            if (mMountedPartMap & (1 << 31)) {
                char mount_pointer[255];

#ifdef FUNCTION_UMS_PARTITION
		if( VOLUME_TYPE_SATA == mVolumeType)
	   {
                if (getLabel())
                    sprintf(mount_pointer, "%s/%s", getMountpoint(), getLabel());
                else
                    sprintf(mount_pointer, "%s", getMountpoint());
		}
		else
		{
		  sprintf(mount_pointer, "%s", getMountpoint());
		}
		
#else
                if (getLabel())
                    sprintf(mount_pointer, "%s/%s", getMountpoint(), getLabel());
                else
                    sprintf(mount_pointer, "%s", getMountpoint());
#endif
                if (doUnmount(mount_pointer, true)) {
                    SLOGE("Failed to unmount %s (%s)", mount_pointer, strerror(errno));
                    return -1;
                }
                rmdir(mount_pointer);
                mMountedPartMap &= ~(1 << 31);
            } else {
                unmountPart(part_index);
            }
        }
    }

    SLOGI("%s unmounted sucessfully", getMountpoint());

    /* If this is an encrypted volume, and we've been asked to undo
     * the crypto mapping, then revert the dm-crypt mapping, and revert
     * the device info to the original values.
     */
    if (revert && isDecrypted()) {
        cryptfs_revert_volume(getLabel());
        revertDeviceInfo();
        SLOGI("Encrypted volume %s reverted successfully", getMountpoint());
    }

    if (getState() != Volume::State_NoMedia)
        setState(Volume::State_Idle);

    mMountedPartMap = 0;

    if (VOLUME_TYPE_UMS == mVolumeType || VOLUME_TYPE_SATA == mVolumeType)
        rmdir(getMountpoint());

    return 0;

    /*
     * Failure handling - try to restore everything back the way it was
     */
fail_recreate_bindmount:
    if (mount(SEC_STG_SECIMGDIR, SEC_ASECDIR, "", MS_BIND, NULL)) {
        SLOGE("Failed to restore bindmount after failure! - Storage will appear offline!");
        goto out_nomedia;
    }
fail_remount_tmpfs:
    if (mount("tmpfs", SEC_STG_SECIMGDIR, "tmpfs", MS_RDONLY, "size=0,mode=0,uid=0,gid=0")) {
        SLOGE("Failed to restore tmpfs after failure! - Storage will appear offline!");
        goto out_nomedia;
    }
fail_republish:
    if (doMoveMount(SEC_STGDIR, getMountpoint(), force)) {
        SLOGE("Failed to republish mount after failure! - Storage will appear offline!");
        goto out_nomedia;
    }

    setState(Volume::State_Mounted);
    return -1;

out_nomedia:
    setState(Volume::State_NoMedia);
    return -1;
}

int Volume::unmountPart(int part_index) {
    char mount_pointer[255];
    const char *label = getDeviceNodesLabel(part_index);


#ifdef HAS_VIRTUAL_CDROM
	if(sLoopMounted==true)
	{
		char * mountpoint;
       mountpoint = (char *)getMountpoint();
		if(strncmp(mountpoint,mloopmapdir,strlen(mountpoint))==0)
		{
			unmountloop(true);     
	 	}
    }
#endif
	
    if ((mMountedPartMap & (1 << part_index)) == 0) {
        return 0;
    }

    if (getState() != Volume::State_Unmounting) {
        setState(Volume::State_Unmounting);
    }

    if (sFakeSdcard && sSdcardMounted && (mSdcardPartitionBit & (1 << part_index))) {
        return unmountFakeSdcard();
    }

    if (label)
        sprintf(mount_pointer, "%s/%s", getMountpoint(), label);
    else
        sprintf(mount_pointer, "%s", getMountpoint());

    SLOGE("%s mountPoint=%s mVolumeType=%u", __func__, mount_pointer, mVolumeType);

    if (doUnmount(mount_pointer, 1)) {
        SLOGE("Failed to unmount %s (%s)", mount_pointer, strerror(errno));
        return -1;
    }

	if (isDecrypted()) {
        cryptfs_revert_volume(mount_pointer);
        revertDeviceInfo();
        SLOGI("Encrypted volume %s reverted successfully", mount_pointer);
    }	
    if (mVolumeType != VOLUME_TYPE_FLASH && mVolumeType != VOLUME_TYPE_SDCARD)
        rmdir(mount_pointer);

    mMountedPartMap &= ~(1 << part_index);
    return 0;
}


int Volume::initializeMbr(const char *deviceNode) {
    struct disk_info dinfo;

    memset(&dinfo, 0, sizeof(dinfo));

    if (!(dinfo.part_lst = (struct part_info *) malloc(MAX_NUM_PARTS * sizeof(struct part_info)))) {
        SLOGE("Failed to malloc prt_lst");
        return -1;
    }

    memset(dinfo.part_lst, 0, MAX_NUM_PARTS * sizeof(struct part_info));
    dinfo.device = strdup(deviceNode);
    dinfo.scheme = PART_SCHEME_MBR;
    dinfo.sect_size = 512;
    dinfo.skip_lba = 2048;
    dinfo.num_lba = 0;
    dinfo.num_parts = 1;

    struct part_info *pinfo = &dinfo.part_lst[0];

    pinfo->name = strdup("android_sdcard");
    pinfo->flags |= PART_ACTIVE_FLAG;
    pinfo->type = PC_PART_TYPE_FAT32;
    pinfo->len_kb = -1;

    int rc = apply_disk_config(&dinfo, 0);

    if (rc) {
        SLOGE("Failed to apply disk configuration (%d)", rc);
        goto out;
    }

 out:
    free(pinfo->name);
    free(dinfo.device);
    free(dinfo.part_lst);

    return rc;
}

int Volume::unmountFakeSdcard() {
    dev_t nodes[MAX_PARTS];
    int n = getDeviceNodes((dev_t *) &nodes, MAX_PARTS);
    if (!n) {
        SLOGE("Failed to get device nodes (%s)\n", strerror(errno));
        return -1;
    }

    int idx;
    if (mSdcardPartitionBit & (1 << 31)) {
        idx = 0;
    }
    else {
        for (idx = 0;  idx < 31; idx ++) {
            if (mSdcardPartitionBit & (1 << idx))
                break;
        }
    }

    NetlinkEvent *evt = newFakeSdcardEvent(1, MAJOR(nodes[idx]), MINOR(nodes[idx]));
    mVm->handleBlockEvent(evt);
    delete evt;
    if (mFakeSdcardLink[0])
        unlink(mFakeSdcardLink);
    mFakeSdcardLink[0] = 0;
    mMountedPartMap &= ~mSdcardPartitionBit;
    mSdcardPartitionBit = 0;
    return 0;
}

NetlinkEvent* Volume::newFakeSdcardEvent(int type, int major, int minor) {
    char msg[512] = {0};
    NetlinkEvent* evt = new NetlinkEvent();
    if (type == 0) //add
        sprintf(msg, MSGFMT_FAKE_ADD_SDCARD, major, minor);
    else
        sprintf(msg, MSGFMT_FAKE_REMOVE_SDCARD, major, minor);

    SLOGD("Fake sdcard event: %s", msg);
    int msglen = strlen(msg);
    int i;
    char *p = msg;
    for (i = 0; i < msglen; i++) {
        if (*p == ' ')
            *p = 0;
        p++;
    }
    evt->decode(msg, msglen);
    return evt;
}

#ifdef HAS_VIRTUAL_CDROM
int Volume::loopsetfd(const char * path)
{
    int fd,file_fd;
     
    if ((fd = open(LOOP_DEV, O_RDWR)) < 0) {
        SLOGE("Unable to open loop0 device (%s)",strerror(errno));
        return -1;
    }

    if ((file_fd = open(path, O_RDWR)) < 0) {
        SLOGE("Unable to open %s (%s)", path, strerror(errno));
        close(fd);
        return -1;
    }

    if (ioctl(fd, LOOP_SET_FD, file_fd) < 0) {
        SLOGE("Error setting up loopback interface (%s)", strerror(errno));
        close(file_fd);
        close(fd);
        return  -1;
    }

    close(fd);
    close(file_fd);

    SLOGD("loopsetfd (%s) ok\n", path);	
    return 0;
}

int Volume::loopclrfd()
{
    int fd;
    int rc=0;
	
    if ((fd = open(LOOP_DEV, O_RDWR)) < 0) {
        SLOGE("Unable to open loop0 device (%s)",strerror(errno));
        return -1;
    }
	
    if (ioctl(fd, LOOP_CLR_FD, 0) < 0) {
        SLOGE("Error setting up loopback interface (%s)", strerror(errno));
        rc = -1;
    }
    close(fd);

    SLOGD("loopclrfd ok\n");	
    return rc;
}

int Volume::mountloop(const char *path) {
    int rc = 0;

	if(sLoopMounted==true)
	{
		SLOGW("loop file already mounted,please umount fist,then mount this file!");
		return -1;
	}
    rc = loopsetfd(path);
    if(rc<0)
    {
        return rc;
    }

    mloopmountdir = strdup(getMountpoint());
    mkdir(mloopmountdir, 0755);
    
	if (doMount(LOOP_DEV, mloopmountdir)) {
		rmdir(mloopmountdir);
	   SLOGW("Volume::loop mount mount failed");
	   free(mloopmountdir);
	   rc = -1;
	}
	else
   {
		SLOGI("Volume::loop mount mounted ok");
		sLoopMounted = true;
		mloopmapdir = strdup(path);
	}
	return rc;
}

int Volume::unmountloop(bool force) {
	if(sLoopMounted==false)
	{
		SLOGW("no loop file mounted");
		return -1;
	}
	if (doUnmount(mloopmountdir, force)) {
 		SLOGE("Failed to unmount %s (%s)", mloopmountdir, strerror(errno));
		return -1;
	}	

	sLoopMounted = false;
	rmdir(mloopmountdir);
	loopclrfd();

	free(mloopmapdir);
	free(mloopmountdir); 
	return 0;
}
#endif

int Volume::getFSLabel(char **outLabel) {
    const char BLKID_PATH[] = "/system/xbin/blkid";
    FILE *blkidOutput;
    char str[255];
    char label[255];
    char devicePath[128];
    char matchStr[255];
    int ret = -3;
    dev_t nodes[1];

    getDeviceNodes((dev_t *)&nodes, 1);
    sprintf(devicePath, "/dev/block/vold/%d:%d", MAJOR(nodes[0]), MINOR(nodes[0]));

    if (access(BLKID_PATH, X_OK)) {
        SLOGW("no blkid");
        return -1;
    }
    blkidOutput = popen(BLKID_PATH, "r");
    if (!blkidOutput) {
        SLOGW("blkid failed");
        return -2;
    }

    snprintf(matchStr, 255, "%s: LABEL=\"%%[^\"]\" ", devicePath);
    /* /dev/block/vold/250:32: UUID="0123-4567"
     * /dev/block/vold/253:1: LABEL="MY SD" UUID="5432-1234"
     * /dev/block/avnftl8: UUID="9988-7766"
     * ...
     */
    while (fgets(str, sizeof(str), blkidOutput) != NULL) {
        int matches;
        str[sizeof(str) - 1] = '\0';
        matches = sscanf(str, matchStr, label);
        if (matches == 1) {
            SLOGV("matched, label=%s", label);
            strcpy(*outLabel, label);
            ret = 0;
            break;
        }
    }
    pclose(blkidOutput);

    return ret;
}
