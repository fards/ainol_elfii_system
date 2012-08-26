#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdint.h>

#include "init.h"
#include "property_service.h"
#include "bootenv.h"
#include "log.h"

typedef unsigned int   uInt; 

char BootenvPartitionName[32]={0};
char PROFIX_UBOOTENV_VAR[32]={0};

#define tole(x) x

#define SPI_PARTITIONS_SIZE   0x2000
#define ENV_SIZE (SPI_PARTITIONS_SIZE - sizeof(uint32_t))

struct erase_info_user {
	uint32_t start;
	uint32_t length;
};


typedef	struct environment_s {
	uint32_t	  crc;			/* CRC32 over data bytes	*/
	unsigned char data[SPI_PARTITIONS_SIZE]; 	/* Environment data		*/
} env_t;


typedef struct env_attribute {
        struct env_attribute *next;
        char key[256];
        char value[1024];
}env_attribute;
	
static struct environment_s env_data;
static struct env_attribute env_attribute_header;
//static char env_arg_buf[SPI_PARTITIONS_SIZE+sizeof(uint32_t)];

static char* default_ubootenv_args = 
"hostname=arm_aio_syscoreenv\0"
"chipname=7366m\0"
"machid=2958\0"
"boardname=m2c_cts\0"
"ethact=Apollo EMAC\0"
"usbtty=cdc_acm\0"
"bootfile=uImage\0"
"baudrate=115200\0"
"console=ttyS1,115200n8\0"
"memsize=1024M\0"
"cpuclock=650M\0"
"gpuclock=187500k\0"
"loadaddr=0x82000000\0"
"ethaddr=00:01:03:73:06:75\0"
"serverip=10.28.8.90\0"
"ipaddr=10.28.8.59\0"
"gatewayip=10.28.8.1\0"
"netmask=255.255.255.0\0"
"recoverymode=0\0"
"nandargs=setenv bootargs mem=${memsize} a9_clk=${cpuclock} clk81=${gpuclock} mac=${ethaddr} lvds=${lvds}\0"
"nandboot=nand info; run nandargs; nand read ${loadaddr} 1800000 480000; bootm ${loadaddr}\0"
"mmcargs=setenv bootargs console=ttyS0,115200n8 root=/dev/cardblksd2 rw rootfstype=ext3 rootwait init=/init mem=${memsize} a9_clk=${cpuclock} clk81=${gpuclock}\0"
"mmcboot=echo Booting from mmc ...; mmcinfo; run mmcargs; mmcinfo 0; fatload mmc 0 ${loadaddr} uImage.android; bootm ${loadaddr}\0"
"recoveryargs=setenv bootargs mem=${memsize} recoverymode=${recoverymode} console=${console} clk81=${gpuclock} lvds=${lvds} \0"
"recoveryspi=run recoveryargs; sf probe 2; sf read ${loadaddr} 40000 1c0000; bootm ${loadaddr}\0"
"recoverynand=run recoveryargs; nand read ${loadaddr} 1000000 800000; bootm ${loadaddr}\0"
"bootcmd=run nandboot\0"
"bootdelay=1\0"
"stdin=serial\0"
"stdout=serial\0"
"stderr=serial\0\0"
;

const uint32_t crc_table[256] = {
tole(0x00000000L), tole(0x77073096L), tole(0xee0e612cL), tole(0x990951baL),
tole(0x076dc419L), tole(0x706af48fL), tole(0xe963a535L), tole(0x9e6495a3L),
tole(0x0edb8832L), tole(0x79dcb8a4L), tole(0xe0d5e91eL), tole(0x97d2d988L),
tole(0x09b64c2bL), tole(0x7eb17cbdL), tole(0xe7b82d07L), tole(0x90bf1d91L),
tole(0x1db71064L), tole(0x6ab020f2L), tole(0xf3b97148L), tole(0x84be41deL),
tole(0x1adad47dL), tole(0x6ddde4ebL), tole(0xf4d4b551L), tole(0x83d385c7L),
tole(0x136c9856L), tole(0x646ba8c0L), tole(0xfd62f97aL), tole(0x8a65c9ecL),
tole(0x14015c4fL), tole(0x63066cd9L), tole(0xfa0f3d63L), tole(0x8d080df5L),
tole(0x3b6e20c8L), tole(0x4c69105eL), tole(0xd56041e4L), tole(0xa2677172L),
tole(0x3c03e4d1L), tole(0x4b04d447L), tole(0xd20d85fdL), tole(0xa50ab56bL),
tole(0x35b5a8faL), tole(0x42b2986cL), tole(0xdbbbc9d6L), tole(0xacbcf940L),
tole(0x32d86ce3L), tole(0x45df5c75L), tole(0xdcd60dcfL), tole(0xabd13d59L),
tole(0x26d930acL), tole(0x51de003aL), tole(0xc8d75180L), tole(0xbfd06116L),
tole(0x21b4f4b5L), tole(0x56b3c423L), tole(0xcfba9599L), tole(0xb8bda50fL),
tole(0x2802b89eL), tole(0x5f058808L), tole(0xc60cd9b2L), tole(0xb10be924L),
tole(0x2f6f7c87L), tole(0x58684c11L), tole(0xc1611dabL), tole(0xb6662d3dL),
tole(0x76dc4190L), tole(0x01db7106L), tole(0x98d220bcL), tole(0xefd5102aL),
tole(0x71b18589L), tole(0x06b6b51fL), tole(0x9fbfe4a5L), tole(0xe8b8d433L),
tole(0x7807c9a2L), tole(0x0f00f934L), tole(0x9609a88eL), tole(0xe10e9818L),
tole(0x7f6a0dbbL), tole(0x086d3d2dL), tole(0x91646c97L), tole(0xe6635c01L),
tole(0x6b6b51f4L), tole(0x1c6c6162L), tole(0x856530d8L), tole(0xf262004eL),
tole(0x6c0695edL), tole(0x1b01a57bL), tole(0x8208f4c1L), tole(0xf50fc457L),
tole(0x65b0d9c6L), tole(0x12b7e950L), tole(0x8bbeb8eaL), tole(0xfcb9887cL),
tole(0x62dd1ddfL), tole(0x15da2d49L), tole(0x8cd37cf3L), tole(0xfbd44c65L),
tole(0x4db26158L), tole(0x3ab551ceL), tole(0xa3bc0074L), tole(0xd4bb30e2L),
tole(0x4adfa541L), tole(0x3dd895d7L), tole(0xa4d1c46dL), tole(0xd3d6f4fbL),
tole(0x4369e96aL), tole(0x346ed9fcL), tole(0xad678846L), tole(0xda60b8d0L),
tole(0x44042d73L), tole(0x33031de5L), tole(0xaa0a4c5fL), tole(0xdd0d7cc9L),
tole(0x5005713cL), tole(0x270241aaL), tole(0xbe0b1010L), tole(0xc90c2086L),
tole(0x5768b525L), tole(0x206f85b3L), tole(0xb966d409L), tole(0xce61e49fL),
tole(0x5edef90eL), tole(0x29d9c998L), tole(0xb0d09822L), tole(0xc7d7a8b4L),
tole(0x59b33d17L), tole(0x2eb40d81L), tole(0xb7bd5c3bL), tole(0xc0ba6cadL),
tole(0xedb88320L), tole(0x9abfb3b6L), tole(0x03b6e20cL), tole(0x74b1d29aL),
tole(0xead54739L), tole(0x9dd277afL), tole(0x04db2615L), tole(0x73dc1683L),
tole(0xe3630b12L), tole(0x94643b84L), tole(0x0d6d6a3eL), tole(0x7a6a5aa8L),
tole(0xe40ecf0bL), tole(0x9309ff9dL), tole(0x0a00ae27L), tole(0x7d079eb1L),
tole(0xf00f9344L), tole(0x8708a3d2L), tole(0x1e01f268L), tole(0x6906c2feL),
tole(0xf762575dL), tole(0x806567cbL), tole(0x196c3671L), tole(0x6e6b06e7L),
tole(0xfed41b76L), tole(0x89d32be0L), tole(0x10da7a5aL), tole(0x67dd4accL),
tole(0xf9b9df6fL), tole(0x8ebeeff9L), tole(0x17b7be43L), tole(0x60b08ed5L),
tole(0xd6d6a3e8L), tole(0xa1d1937eL), tole(0x38d8c2c4L), tole(0x4fdff252L),
tole(0xd1bb67f1L), tole(0xa6bc5767L), tole(0x3fb506ddL), tole(0x48b2364bL),
tole(0xd80d2bdaL), tole(0xaf0a1b4cL), tole(0x36034af6L), tole(0x41047a60L),
tole(0xdf60efc3L), tole(0xa867df55L), tole(0x316e8eefL), tole(0x4669be79L),
tole(0xcb61b38cL), tole(0xbc66831aL), tole(0x256fd2a0L), tole(0x5268e236L),
tole(0xcc0c7795L), tole(0xbb0b4703L), tole(0x220216b9L), tole(0x5505262fL),
tole(0xc5ba3bbeL), tole(0xb2bd0b28L), tole(0x2bb45a92L), tole(0x5cb36a04L),
tole(0xc2d7ffa7L), tole(0xb5d0cf31L), tole(0x2cd99e8bL), tole(0x5bdeae1dL),
tole(0x9b64c2b0L), tole(0xec63f226L), tole(0x756aa39cL), tole(0x026d930aL),
tole(0x9c0906a9L), tole(0xeb0e363fL), tole(0x72076785L), tole(0x05005713L),
tole(0x95bf4a82L), tole(0xe2b87a14L), tole(0x7bb12baeL), tole(0x0cb61b38L),
tole(0x92d28e9bL), tole(0xe5d5be0dL), tole(0x7cdcefb7L), tole(0x0bdbdf21L),
tole(0x86d3d2d4L), tole(0xf1d4e242L), tole(0x68ddb3f8L), tole(0x1fda836eL),
tole(0x81be16cdL), tole(0xf6b9265bL), tole(0x6fb077e1L), tole(0x18b74777L),
tole(0x88085ae6L), tole(0xff0f6a70L), tole(0x66063bcaL), tole(0x11010b5cL),
tole(0x8f659effL), tole(0xf862ae69L), tole(0x616bffd3L), tole(0x166ccf45L),
tole(0xa00ae278L), tole(0xd70dd2eeL), tole(0x4e048354L), tole(0x3903b3c2L),
tole(0xa7672661L), tole(0xd06016f7L), tole(0x4969474dL), tole(0x3e6e77dbL),
tole(0xaed16a4aL), tole(0xd9d65adcL), tole(0x40df0b66L), tole(0x37d83bf0L),
tole(0xa9bcae53L), tole(0xdebb9ec5L), tole(0x47b2cf7fL), tole(0x30b5ffe9L),
tole(0xbdbdf21cL), tole(0xcabac28aL), tole(0x53b39330L), tole(0x24b4a3a6L),
tole(0xbad03605L), tole(0xcdd70693L), tole(0x54de5729L), tole(0x23d967bfL),
tole(0xb3667a2eL), tole(0xc4614ab8L), tole(0x5d681b02L), tole(0x2a6f2b94L),
tole(0xb40bbe37L), tole(0xc30c8ea1L), tole(0x5a05df1bL), tole(0x2d02ef8dL)
};

#define DO_CRC(x) crc = tab[(crc ^ (x)) & 255] ^ (crc >> 8)
uint32_t crc32_no_comp(uint32_t crc, unsigned char *buf, uInt len)
{
    const uint32_t *tab = crc_table;
    const uint32_t *b =(const uint32_t *)buf;
    size_t rem_len;
    /* Align it */
    if (((long)b) & 3 && len) {
	 uint8_t *p = (uint8_t *)b;
	 do {
	      DO_CRC(*p++);
	 } while ((--len) && ((long)p)&3);
	 b = (uint32_t *)p;
    }

    rem_len = len & 3;
    len = len >> 2;
    for (--b; len; --len) {
	 /* load data 32 bits wide, xor data 32 bits wide. */
	 crc ^= *++b; /* use pre increment for speed */
	 DO_CRC(0);
	 DO_CRC(0);
	 DO_CRC(0);
	 DO_CRC(0);
    }
    len = rem_len;
    /* And the last few bytes */
    if (len) {
	 uint8_t *p = (uint8_t *)(b + 1) - 1;
	 do {
	      DO_CRC(*++p); /* use pre increment for speed */
	 } while (--len);
    }
    return crc;
}
uint32_t crc32 (uint32_t crc, unsigned char *p, uInt len)
{
     return crc32_no_comp(crc ^ 0xffffffffL, p, len) ^ 0xffffffffL;
}

/*************************for demo uboot arg areas write uboot args read and write*********************/

/* Parse a session attribute */
static env_attribute *
env_parse_attribute(char *data)
{
    char *value,*key; 
	char *line;
	char *proc = data;
	char *next_proc;
	env_attribute *attr = &env_attribute_header;
    int n_char,n_char_end;
	if(proc == NULL){
		ERROR("---ERROR:What are you doing? input data is NULL\n");
		return NULL;
	}
	memset(attr, 0, sizeof(env_attribute));
	
	do{	        
		next_proc = proc+strlen(proc)+sizeof(char);
		//NOTICE("process %s\n",proc);
        key = strchr(proc, (int)'=');
		if(key != NULL){
			*key=0;
			strcpy(attr->key, proc);
			strcpy(attr->value, key+sizeof(char));
		}else{
			ERROR("error need '=' skip this value\n");
		}
		if(!(*next_proc)){
			//NOTICE("process end \n");
			break;
		}
		proc = next_proc;			
		/****************************************************/
		attr->next =(env_attribute *)malloc(sizeof(env_attribute));
		if(attr->next == NULL){
			ERROR("exit malloc error \n");
			break;
		}
		memset(attr->next, 0, sizeof(env_attribute));
		attr = 	attr->next;
	}while(1);

	//printf("*********key: [%s]\n",env_attribute_header.next->key);
    return &env_attribute_header;
}

/*  attribute revert to sava data*/
static char *
env_revert_attribute(env_attribute *attr)
{
	int len;
	char *data = (char*)env_data.data;
	
	memset(&env_data,0,sizeof(env_data));
	if(attr == NULL){
		printf("---ERROR:What are you doing? input attr is NULL\n");
	}
	do{
	//for(;attr;attr=attr->next){
		//printf("key:[%s]\n",attr->key);
		len=sprintf(data,"%s=%s",attr->key,attr->value);		
		if(len < (int)(sizeof(char)*3)){
			printf("----Invalid data---- \n");
		}
		else
			data += len+sizeof(char);
		attr=attr->next;
	}while(attr);

	return (char*)env_data.data;
}

/*
 * MEMERASE
 */
 #ifndef UBOOTENV_SAVE_IN_NAND
 #define MEMERASE		_IOW('M', 2, struct erase_info_user)
 
static int memerase (int fd,struct erase_info_user *erase)
{
	return (ioctl (fd,MEMERASE,erase));
}
#endif

int
read_bootenv(char *bootarg_dev)
{
	int fd;
	int ret;
	uint32_t crc_calc;
	env_attribute *attr;
	char * data;

	if ((fd = open (bootarg_dev,O_RDONLY)) < 0)
	{
		printf("----open devices error\n");
		return -1;
	}
	
 	memset((void*)&env_data,0,sizeof(env_data));
 	ret = read(fd ,(void *)&env_data, SPI_PARTITIONS_SIZE);
 	if(ret == SPI_PARTITIONS_SIZE)
 	{
 		  crc_calc = crc32 (0,env_data.data, ENV_SIZE); 
		  if(crc_calc != env_data.crc){	
			  printf("----CRC Check ERROR save_crc=%08x,calc_crc = %08x \n"
													,env_data.crc,crc_calc);
			  close(fd);
			  return -4;
		  }
		  /*else{
			  printf("CRC Check OK = %08x \n",crc_calc);
		  }*/
		  attr = env_parse_attribute((char*)env_data.data);
		  if(attr == NULL){
		  	close(fd);
			return -2;
		  }
		  /*************************
		  while(attr != NULL){		  	
			printf("key:   [%s]\n",attr->key);
			printf("value: [%s]\n\n",attr->value);
		  	
			attr = attr->next;
		  }
		  *******************************/
 	}else{
 		  printf("----read error %d \n",ret);
		  close(fd);
		  return -3;
 	}
	close(fd);
	return 0;	
}

void  bootenv_print(void)
{
	env_attribute *attr=&env_attribute_header;
	while(attr != NULL){		  	
		printf("key:   [%s]\n",attr->key);
		printf("value: [%s]\n\n",attr->value);
			
		attr = attr->next;
	}
}

const char * bootenv_get_value(const char * key)
{
	env_attribute *attr=&env_attribute_header;
	while(attr){
		if(!strcmp(key,attr->key)){
			return attr->value;
		}
		attr = attr->next;
	}
	return NULL;
}
/*
creat_args_flag : if true , if envvalue don't exists Creat it .
					if false , if envvalue don't exists just exit .
*/
int bootenv_set_value(const char * key,  const char * value,int creat_args_flag)
{
	env_attribute *attr=&env_attribute_header;
	env_attribute *last = attr;
	while(attr){
		if(!strcmp(key,attr->key)){
			strcpy(attr->value,value);
			return 2;
		}
		last = attr;
		attr = attr->next;
	}
	
	if(creat_args_flag){
		NOTICE("ubootenv.var.%s not found, create it.\n", key);
		/*******Creat a New args*********************/
		attr =(env_attribute *)malloc(sizeof(env_attribute));
		last->next = attr;
		memset(attr, 0, sizeof(env_attribute));
		strcpy(attr->key,key);
		strcpy(attr->value,value);
		return 1;
	}else
		return 0;
}
int save_bootenv(char * dev)
{
	int fd;
	int err;
	struct erase_info_user erase;
	
	env_revert_attribute(&env_attribute_header);
	env_data.crc = crc32(0, env_data.data, ENV_SIZE);

	printf("save new crc value [0x%08x]\n",env_data.crc);
	
	if ((fd = open (dev, O_RDWR)) < 0)
	{
		printf("----open devices error\n");
		return -1;
	}

	erase.start = 0;
	erase.length = SPI_PARTITIONS_SIZE;
	#ifndef UBOOTENV_SAVE_IN_NAND
	err = memerase (fd,&erase);
	if (err < 0)
	{
		printf ("MEMERASE ERROR %d\n",err);
	        close(fd);
		return  -2;
	}
	#endif
 	err = write(fd ,(void *)&env_data, SPI_PARTITIONS_SIZE);	
	close(fd);
	if (err < 0)
	{ 
		printf ("----ERROR  write, size %d \n",SPI_PARTITIONS_SIZE);
		return -3;
	}
	
	return 0;
}

int is_bootenv_varible(const char* prop_name) 
{
	if (!prop_name || !(*prop_name))
		return 0;

	if (!(*PROFIX_UBOOTENV_VAR))
		return 0;

	if (strncmp(prop_name, PROFIX_UBOOTENV_VAR, strlen(PROFIX_UBOOTENV_VAR)) == 0
		&& strlen(prop_name) > strlen(PROFIX_UBOOTENV_VAR) )
		return 1;

	return 0;
}

static void init_bootenv_prop(const char *key, const char *value, void *cookie)
{
	if (is_bootenv_varible(key)) {
		const char* varible_name = key + strlen(PROFIX_UBOOTENV_VAR);
		const char *varible_value = bootenv_get_value(varible_name);
		if (!varible_value)
			varible_value = "";
		if (strcmp(varible_value, value)) {
			property_set(key, varible_value);
			(*((int*)cookie))++;
		}		
	}
}

#if BOOT_ARGS_CHECK

char mac_address[20] = {0};
char licence[36] = {0};
char device_id[42] = {0};
#define BURN_ARGS_SIZE  128
#define BURN_ARGS_FILENAME "/param/burn_args.arg"
#define ARGS_ETHADDR "ethaddr"
#define ARGS_ETHADDR_WHOLE "ubootenv.var.ethaddr"
#define ARGS_LICENCE "igrsid"
#define ARGS_LICENCE_WHOLE "ubootenv.var.igrsid"
#define ARGS_DEVICE_ID "huanid"
#define ARGS_DEVICE_ID_HOLE "ubootenv.var.huanid"


typedef	struct burn_args {
	uint32_t	  crc;			/* CRC32 over data bytes	*/
	unsigned char data[BURN_ARGS_SIZE]; 	/* Environment data		*/
} burn_t;

static struct burn_args  burn_args_data ;


int read_args_from_file()
{
    char *data ;
    uint32_t crc_calc;
    char * write_buff = mac_address ;
    int file_size = 0 , read_pos = 0 , write_pos = 0 ;
    data = read_file( BURN_ARGS_FILENAME , &file_size);
    
    if (!data)
    {
    	NOTICE( " E03LOG %s failed!\n" , BURN_ARGS_FILENAME );
    	return -1;
    }
    else
    {
    	NOTICE( " E03LOG read_default_boot_args success size: [%d]  burn_args_data size : [%d] ;  %s \n" , file_size , sizeof(burn_args_data) , data );
	if( file_size > sizeof(burn_args_data) )
	{
	    	NOTICE( " E03LOG file size :[%d] > burn_args_data size: [%d] \n" , file_size , sizeof(burn_args_data) );
		//return -3 ;
	}
	memset((void*)&burn_args_data,0,sizeof(burn_args_data));
    	memcpy( (void *)&burn_args_data , data , sizeof(burn_args_data) );
	crc_calc = crc32 (0,burn_args_data.data, BURN_ARGS_SIZE); 
	if(crc_calc != burn_args_data.crc)
	{
	    	NOTICE( " E03LOG check args failed! file crs32:[%d]  ,  real crs32:[%d]\n" , burn_args_data.crc , crc_calc );
		return -2 ;
	}
	else
	{
	    	NOTICE( " E03LOG check args accordant ! file crs32:[%d]  ,  real crs32:[%d]\n" , burn_args_data.crc , crc_calc );
	}
    }
    for( read_pos = 0 ; read_pos < file_size ; read_pos++ )
    {
    	if( 0x0A == burn_args_data.data[read_pos] )
    	{
    		if( write_buff == mac_address )
	 	{
		    	NOTICE( " E03LOG mac of file %s \n" , write_buff  );
	 		write_buff = licence ;
		}
		else if( write_buff == licence  )
		{
		    	NOTICE( " E03LOG licence of file %s \n" , write_buff  );
			write_buff = device_id ;
		}
		else 
		{
		    	NOTICE( " E03LOG device_id of file %s \n" , write_buff  );
			break;
		}
		write_pos = 0 ;
    		continue ;
    	}
        write_buff[write_pos] = burn_args_data.data[read_pos] ;
	write_pos++ ;
    }
    return 0;
}

int write_args_to_file()
{
	int fd;
	int err;
	if ((fd = open (BURN_ARGS_FILENAME, O_RDWR| O_CREAT )) < 0)
	{
		NOTICE( " E03LOG ----open devices error\n");
		return -1;
	}

	if(lseek(fd, 0, SEEK_SET) != 0) 
	{
		NOTICE( " E03LOG lseek ERROR %d\n",err);
	}
     	memset((void*)&burn_args_data,0,sizeof(burn_args_data));
	sprintf( burn_args_data.data , "%s\n%s\n%s\n", bootenv_get_value(ARGS_ETHADDR),bootenv_get_value(ARGS_LICENCE),bootenv_get_value(ARGS_DEVICE_ID)) ;
	burn_args_data.crc = crc32 (0,burn_args_data.data, BURN_ARGS_SIZE); 

	NOTICE( " E03LOG  write args crc: [%d]   \n %s \n " , burn_args_data.crc , burn_args_data.data ) ;
	err = write(fd ,(void *)&burn_args_data, sizeof(burn_args_data));	
	close(fd);
	if (err < 0)
	{ 
		NOTICE( " E03LOG write args----ERROR  write, size %d \n",err);
		return -3;
	}
	else
	{
		NOTICE( " E03LOG write args----success , size %d \n",err);
	}
	return 0 ;
}

void check_boot_args()
{
	int ret = read_args_from_file();
	char * p_ethaddr = bootenv_get_value(ARGS_ETHADDR) ;
	char * p_licence = bootenv_get_value(ARGS_LICENCE);
	char * p_device_id = bootenv_get_value(ARGS_DEVICE_ID) ;
	
	if( 0 != ret )
	{
		if( NULL != p_ethaddr && NULL != p_licence && NULL != p_device_id )
			write_args_to_file();
		NOTICE( " E03LOG read args from file failed . write boot args to file \n" ) ;
		return ;
	}
	 
	if( NULL == p_ethaddr || 0 != strcmp( mac_address ,  p_ethaddr ))
	{
		NOTICE( " E03LOG check mac address failed . file: %s  boot: %s \n" , mac_address ,  bootenv_get_value(ARGS_ETHADDR)  ) ;
		update_bootenv_varible( ARGS_ETHADDR_WHOLE, mac_address );
	}
	else
	{
		NOTICE( " E03LOG check mac address accordant . file: %s  boot: %s \n" , mac_address ,  bootenv_get_value(ARGS_ETHADDR)  ) ;
	}
	
	if( NULL == p_licence || 0 != strcmp( licence,  p_licence ))
	{
		NOTICE( " E03LOG check licence failed . file: %s  boot: %s \n" , licence ,  bootenv_get_value(ARGS_LICENCE)  ) ;
		update_bootenv_varible( ARGS_LICENCE_WHOLE, licence );
	}
	else
	{
		NOTICE( " E03LOG check licence accordant . file: %s  boot: %s \n" , licence ,  bootenv_get_value(ARGS_LICENCE)  ) ;
	}
	
	if( NULL == p_device_id || 0 != strcmp( device_id,  p_device_id  ) )
	{
		NOTICE( " E03LOG check device_id failed . file: %s  boot: %s \n" , device_id ,  bootenv_get_value(ARGS_DEVICE_ID)  ) ;
		update_bootenv_varible( ARGS_DEVICE_ID_HOLE, device_id );
	}
	else
	{
		NOTICE( " E03LOG check device_id accordant . file: %s  boot: %s \n" , device_id ,  bootenv_get_value(ARGS_DEVICE_ID)  ) ;
	}

}

void set_default_boot_args()
{
    char *data;
    int file_size = 0 , i = 0 ;
    data = read_file("/param/default_bootargs.args", &file_size);
    if (!data)
    {
    	NOTICE( " E03LOG read /default_bootargs.args failed!\n" );
    	return ;
    }
    else
    {
    	NOTICE( " E03LOG read /default_bootargs.args success size %d !\n\n  %s \n" , file_size , data);
    }
    for( ; i < file_size ; i++ )
    {
    	if( 0x0A == data[i] )
    	{
    		data[i] = 0x0 ;
		NOTICE( " E03LOG set %d 0x0A to 0x00 !\n" , i );
    	}
    }
    default_ubootenv_args = data ;
}

#endif

#define MAX_UBOOT_RWRETRY 5

int init_bootenv_varibles(char *dev) {
     if (!dev || !(*dev)) 
	    return -1;

     strncpy(BootenvPartitionName, dev, 31);
	 
	int  ret = -1; 
	int i = 0;
	while (i < MAX_UBOOT_RWRETRY && ret < 0) {
		i ++;
		ret = read_bootenv(dev);
		if (ret < 0) 
		    ERROR("Cannot read %s: %d.\n", dev, ret);
	}

	if (i >= MAX_UBOOT_RWRETRY) {
#if BOOT_ARGS_CHECK
		NOTICE( " E03LOG ----set_default_boot_args\n");
    		set_default_boot_args();
#endif
		NOTICE("Cannot read ubootenv varibles - all use default.\n");
		char envbuf[4*1024];
		//memset(envbuf, 0, sizeof(envbuf));
		memcpy(envbuf, default_ubootenv_args, sizeof(envbuf));
		env_parse_attribute(envbuf);
	}

    const char* prefix = property_get("ro.ubootenv.varible.prefix");
    if (!prefix) {
	prefix = "ubootenv.var";
	property_set("ro.ubootenv.varible.prefix",  prefix);
    }
    
    if (!(*prefix)) {
	NOTICE("Cannot r/w ubootenv varibles - prefix is empty.\n");
	return -3;
    }
	
    if (strlen(prefix) > 16) {
	NOTICE("Cannot r/w ubootenv varibles - prefix length > 16.\n");
	return -4;
    }

    sprintf(PROFIX_UBOOTENV_VAR, "%s.", prefix);
    INFO("ubootenv varible prefix is: %s\n", prefix);
	
	int count = 0;
	property_list(init_bootenv_prop, (void*)&count);

	char bootcmd[32];
	sprintf(bootcmd, "%s.bootcmd", prefix);
	if (property_get(bootcmd) == NULL) {
		const char* value = bootenv_get_value("bootcmd");
		property_set(bootcmd, value);
		count ++;
	}
	
	INFO("Get %d varibles from %s succeed!\n", count, BootenvPartitionName);	
	return 0;
}

int update_bootenv_varible(const char* name, const char* value)
{
	if (!BootenvPartitionName[0]) {
 	      ERROR("Cannot update %s: ubootenv partition not found.\n", name);
		return -1;
	}
	
	const char* varible_name = 0;
       if (strcmp(name, "ubootenv.var.bootcmd") == 0) {
	   	varible_name = "bootcmd";
       }
	else {
		if (!is_bootenv_varible(name)) {
			//should assert here. 
 		      ERROR("%s is not a ubootenv varible.\n", name);
			return -2;
		}
		varible_name = name + strlen(PROFIX_UBOOTENV_VAR);
	}

	const char *varible_value = bootenv_get_value(varible_name);
	if (!varible_value)
		varible_value = "";
		
	if (!strcmp(value, varible_value)) 
		return 0;

	bootenv_set_value(varible_name, value, 1);
	
 	int i = 0;
	int ret = -1;
	while (i < MAX_UBOOT_RWRETRY && ret < 0) {
		i ++;
		ret = save_bootenv(BootenvPartitionName);
		if (ret < 0) 
		    ERROR("Cannot write %s: %d.\n", BootenvPartitionName, ret);
	}

	if (i < MAX_UBOOT_RWRETRY) {
		INFO("Save ubootenv to %s succeed!\n",  BootenvPartitionName);
	}

#if BOOT_ARGS_CHECK
	NOTICE( " E03LOG ----update_bootenv_varible %s = %s \n" , name , value );
	if (strcmp(name, ARGS_ETHADDR_WHOLE) == 0 ||
   	     strcmp(name, ARGS_LICENCE_WHOLE) == 0 ||
   	     strcmp(name, ARGS_DEVICE_ID_HOLE) == 0 )
	{
		char * p_ethaddr = bootenv_get_value(ARGS_ETHADDR) ;
		char * p_licence = bootenv_get_value(ARGS_LICENCE);
		char * p_device_id = bootenv_get_value(ARGS_DEVICE_ID) ;
		NOTICE( " E03LOG ----update_bootenv_varible write_args_to_file   \n");

		if( NULL != p_ethaddr && NULL != p_licence && NULL != p_device_id )
			write_args_to_file();
	}
#endif
	return ret;
}


