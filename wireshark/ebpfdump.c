/*
 *
 * (C) 2019 - ntop.org
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lessed General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 */

#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <getopt.h>
#include <signal.h>
#include <stdlib.h>
#include <time.h>
#include <pcap.h>
#include "pcapio.c"

#include "ebpf_flow.h"

#define offsetof(st, m) __builtin_offsetof(st, m)

#define EBPFDUMP_INTERFACE       "ebpf"

#define SOCKET_LIBEBPF           2019
#define EXIT_SUCCESS             0

#define EBPFDUMP_MAX_NBPF_LEN    8192
#define EBPFDUMP_MAX_DATE_LEN    26
#define EBPFDUMP_MAX_NAME_LEN    4096

#define EBPFDUMP_VERSION_MAJOR   "0"
#define EBPFDUMP_VERSION_MINOR   "1"
#define EBPFDUMP_VERSION_RELEASE "0"

#define EXTCAP_OPT_LIST_INTERFACES	'l'
#define EXTCAP_OPT_VERSION		'v'
#define EXTCAP_OPT_LIST_DLTS		'L'
#define EXTCAP_OPT_INTERFACE		'i'
#define EXTCAP_OPT_CONFIG		'c'
#define EXTCAP_OPT_CAPTURE		'C'
#define EXTCAP_OPT_FIFO			'F'
#define EXTCAP_OPT_DEBUG		'D'
#define EBPFDUMP_OPT_HELP		'h'
#define EBPFDUMP_OPT_NAME		'n'
#define EBPFDUMP_OPT_CUSTOM_NAME	'N'

static struct option longopts[] = {
  /* mandatory extcap options */
  { "extcap-interfaces",	no_argument, 		NULL, EXTCAP_OPT_LIST_INTERFACES },
  { "extcap-version", 		optional_argument, 	NULL, EXTCAP_OPT_VERSION },
  { "extcap-dlts", 		no_argument, 		NULL, EXTCAP_OPT_LIST_DLTS },
  { "extcap-interface", 	required_argument, 	NULL, EXTCAP_OPT_INTERFACE },
  { "extcap-config", 		no_argument, 		NULL, EXTCAP_OPT_CONFIG },
  { "capture", 			no_argument, 		NULL, EXTCAP_OPT_CAPTURE },
  { "fifo", 			required_argument, 	NULL, EXTCAP_OPT_FIFO },
  { "debug", 			optional_argument, 	NULL, EXTCAP_OPT_DEBUG },

  /* custom extcap options */
  { "help", 			no_argument, 		NULL, EBPFDUMP_OPT_HELP },
  { "name", 			required_argument,	NULL, EBPFDUMP_OPT_NAME },
  { "custom-name", 		required_argument, 	NULL, EBPFDUMP_OPT_CUSTOM_NAME },

  {0, 0, 0, 0}
};

typedef struct _extcap_interface {
  const char * interface;
  const char * description;
  u_int16_t dlt;
  const char * dltname;
  const char * dltdescription;
} extcap_interface;

#define DLT_EN10MB 1

static extcap_interface extcap_interfaces[] = {
  { EBPFDUMP_INTERFACE, "eBPF interface", DLT_EN10MB, NULL, "The EN10MB Ethernet2 DLT" },
};

#define MAX_NUM_INT 32

static size_t extcap_interfaces_num = sizeof(extcap_interfaces) / sizeof(extcap_interface);
static u_int8_t debug = 0;
static char *extcap_selected_interface   = NULL;
static char *extcap_capture_fifo         = NULL;
static FILE *fp                          = NULL;
static char *all_interfaces[MAX_NUM_INT] = { NULL };
static u_int8_t num_all_interfaces       = 0;

/* ***************************************************** */

void sigproc(int sig) {
  fprintf(stdout, "Exiting...");
  fflush(stdout);
  exit(0);
}

/* ***************************************************** */

void extcap_version() {
  /* Print version */
  printf("extcap {version=%s.%s.%s}\n",
	 EBPFDUMP_VERSION_MAJOR, EBPFDUMP_VERSION_MINOR,
	 EBPFDUMP_VERSION_RELEASE);
}

/* ***************************************************** */

void kubectl_list_interfaces() {
  FILE *fd;
  int rc ;
  struct stat statbuf;
  const char *kcmd;
  char cmd[256];

  if(stat("/snap/bin/microk8s.kubectl", &statbuf) == 0)
    kcmd = "/snap/bin/microk8s.kubectl";
  else if(stat("/usr/bin/kubectl", &statbuf) == 0)
    kcmd = "/usr/bin/kubectl";
  else
    return; /* No kubectk */

  snprintf(cmd, sizeof(cmd), "%s get namespace -o 'jsonpath={.items[*].metadata.name}'",
	   kcmd);

  if(debug) printf("[DEBUG][%s:%u] Executing %s\n", __FILE__, __LINE__, cmd);

  if((fd = popen(cmd, "r")) != NULL) {
    char line[1024];

    if(fgets(line, sizeof(line)-1, (FILE*) fd)) {
      char *tmp, *ns = strtok_r(line, " ", &tmp);

      if(debug) printf("[DEBUG][%s:%u] Read %s\n", __FILE__, __LINE__, line);

      while(ns) {
	FILE *fd1;

	snprintf(cmd, sizeof(cmd), "%s get pod --namespace=%s -o jsonpath='{.items[*].metadata.name}'",
		 kcmd, ns);

	if(debug) printf("[DEBUG][%s:%u] Executing %s\n", __FILE__, __LINE__, cmd);

	if((fd1 = popen(cmd, "r")) != NULL) {
	  char pod[512];

	  while(fgets(pod, sizeof(pod)-1, (FILE*)fd1)) {
	    char *tmp, *ns1;
	    FILE *fd2;

	    if(debug) printf("[DEBUG][%s:%u] Read %s\n", __FILE__, __LINE__, pod);

	    ns1 = strtok_r(pod, " ", &tmp);

	    while(ns1 != NULL) {
	      snprintf(cmd, sizeof(cmd),
		       "%s exec %s --namespace=%s --  cat /sys/class/net/eth0/iflink 2>1 /dev/null",
		       kcmd, ns1, ns);

	      if(debug) printf("[DEBUG][%s:%u] Executing %s\n", __FILE__, __LINE__, cmd);

	      if((fd2 = popen(cmd, "r")) != NULL) {
		char ids[32];

		while(fgets(ids, sizeof(ids)-1, (FILE*) fd2)) {
		  FILE *fd3;

		  if(debug) printf("[DEBUG][%s:%u] Read %s\n", __FILE__, __LINE__, ids);

		  snprintf(cmd, sizeof(cmd), "ip -o link|grep ^%d:|cut -d ':' -f 2|cut -d '@' -f 1|tr -d '[:blank:]' | sed 's/\\n//g'",
			   atoi(ids));

		  if(debug) printf("[DEBUG][%s:%u] Executing %s\n", __FILE__, __LINE__, cmd);

		  if((fd3 = popen(cmd, "r")) != NULL) {
		    char ifname[32];

		    while(fgets(ifname, sizeof(ifname)-1, (FILE*) fd3)) {
		      if(debug) printf("[DEBUG][%s:%u] Read %s\n", __FILE__, __LINE__, ifname);

		      ifname[strlen(ifname)-1] = '\0';
		      // printf("[ns: %s][pod: %s][iflink: %d][ifname: %s]\n", ns1, pod, atoi(ids), ifname);
		      printf("interface {value=%s}{display=Pod %s, Namespace %s}\n", ifname, ns1, ns);

		      if(num_all_interfaces < MAX_NUM_INT)
			all_interfaces[num_all_interfaces++] = strdup(ifname);
		    }

		    fclose(fd3);
		  }
		}

		fclose(fd2);
	      }

	      ns1 = strtok_r(NULL, " ", &tmp);
	      
	      if(debug) printf("[DEBUG][%s:%u] Next NS %s\n", __FILE__, __LINE__, ns1 ? ns1 : "<NULL>");
	    }
	  }

	  fclose(fd1);
	}

	ns = strtok_r(NULL, " ", &tmp);
      }
    }

    fclose(fd);
  }
}

/* ***************************************************** */

void print_pcap_interfaces() {
  char errbuf[PCAP_ERRBUF_SIZE];
  pcap_if_t *devpointer;
  
  if(pcap_findalldevs(&devpointer, errbuf) == 0) {
    int i = 0;

    while(devpointer) {
      if(devpointer->description == NULL) {
	u_int8_t found = 0, i;

	for(i=0; i<num_all_interfaces; i++)
	  if(strcmp(all_interfaces[i], devpointer->name) == 0) {
	    found = 1;
	    break;
	  }
	
	if(!found)
	  printf("interface {value=%s}{display=%s}\n",
		 devpointer->name, devpointer->name);
      }
      
      devpointer = devpointer->next;
    }
  }
}

/* ***************************************************** */

void extcap_list_interfaces() {
  u_int i;

  for(i = 0; i < extcap_interfaces_num; i++)
    printf("interface {value=%s}{display=%s}\n",
	   extcap_interfaces[i].interface,
	   extcap_interfaces[i].description);

  kubectl_list_interfaces();

  /* Print additional interfaces */
  print_pcap_interfaces();

  for(i=0; i<num_all_interfaces; i++)
    free(all_interfaces[i]);
}

/* ***************************************************** */

void extcap_dlts() {
  int i;

  if(!extcap_selected_interface) return;
  for(i = 0; i < extcap_interfaces_num; i++) {
    extcap_interface *eif = &extcap_interfaces[i];

    if(!strncmp(extcap_selected_interface, eif->interface, strlen(eif->interface))) {
      printf("dlt {number=%u}{name=%s}{display=%s}\n",
	     eif->dlt, eif->interface, eif->dltdescription);
      break;
    }
  }
}

/* ***************************************************** */

int exec_head(const char *bin, char *line, size_t line_len) {
  FILE *fp;

  fp = popen(bin, "r");

  if(fp == NULL)
    return -1;

  if(fgets(line, line_len-1, fp) == NULL) {
    pclose(fp);
    return -1;
  }

  pclose(fp);
  return 0;
}

/* ***************************************************** */

float wireshark_version() {
  char line[1035];
  char *version, *rev;
  float v = 0;

  if(exec_head("/usr/bin/wireshark -v", line, sizeof(line)) != 0 &&
      exec_head("/usr/local/bin/wireshark -v", line, sizeof(line)) != 0)
    return 0;

  version = strchr(line, ' ');
  if(version == NULL) return 0;
  version++;
  rev = strchr(version, '.');
  if(rev == NULL) return 0;
  rev++;
  rev = strchr(rev, '.');
  if(rev == NULL) return 0;
  *rev = '\0';

  sscanf(version, "%f", &v);

  return v;
}

/* ***************************************************** */

void extcap_config() {
  u_int argidx = 0;

  if(!extcap_selected_interface) return;

  if(!strncmp(extcap_selected_interface, EBPFDUMP_INTERFACE, strlen(EBPFDUMP_INTERFACE))) {
    u_int nameidx;

    nameidx = argidx;
    printf("arg {number=%u}{call=--name}"
	   "{display=Interface Name}{type=radio}"
	   "{tooltip=The interface name}\n", argidx++);
  }
}

/* ***************************************************** */

static void ebpf_process_event(void* t_bpfctx, void* t_data, int t_datasize) {
  eBPFevent *e = (eBPFevent*)t_data;
  u_int len = sizeof(eBPFevent)+4;
  char buf[len];
  eBPFevent *event = (eBPFevent*)&buf[4];
  struct timespec tp;
  struct timeval now;
  u_int64_t bytes_written = 0;
  int err;
  u_int32_t *null_sock_type = (u_int32_t*)buf;

  /* Copy needed as ebpf_preprocess_event will modify the memory */
  memcpy(event, e, sizeof(eBPFevent));
  ebpf_preprocess_event(event);

  gettimeofday(&now, NULL);

  *null_sock_type = htonl(SOCKET_LIBEBPF);

  if(extcap_selected_interface != NULL) {
    /* 
       We are capturing from a physical interface and here we need
       to glue events with packets
    */

    if(strcmp(event->ifname, extcap_selected_interface) == 0) {
      printf("[%s][%s][IPv4/%s][pid/tid: %u/%u [%s], uid/gid: %u/%u][father pid/tid: %u/%u [%s], uid/gid: %u/%u]",
	     event->ifname, event->sent_packet ? "Sent" : "Rcvd",
	     (event->proto == IPPROTO_TCP) ? "TCP" : "UDP",
	     event->proc.pid, event->proc.tid,
	     (event->proc.full_task_path == NULL) ? event->proc.task : event->proc.full_task_path,
	     event->proc.uid, event->proc.gid,
	     event->father.pid, event->father.tid,
	     (event->father.full_task_path == NULL) ? event->father.task : event->father.full_task_path,
	     event->father.uid, event->father.gid);
	
      if(event->container_id[0] != '\0') {
	printf("[containerID: %s]", event->container_id);
      
	if(event->docker.name != NULL)
	  printf("[docker_name: %s]", event->docker.name);
      
	if(event->kube.ns)  printf("[kube_name: %s]", event->kube.name);
	if(event->kube.pod) printf("[kube_pod: %s]",  event->kube.pod);
	if(event->kube.ns)  printf("[kube_ns: %s]",   event->kube.ns);
      }
      
      printf("\n");
    } else
      printf("Skipping event for interface %s\n", event->ifname);
  } else {
    if(!libpcap_write_packet(fp, now.tv_sec, now.tv_usec, len, len,
			     (const u_int8_t*)buf, &bytes_written, &err)) {
      time_t now = time(NULL);
      fprintf(stderr, "Error while writing packet @ %s", ctime(&now));
    }
  }
  
  fflush(fp); /* Flush buffer */
  ebpf_free_event(event);
}

/* ***************************************************** */

void pcap_processs_packet(u_char *_deviceId,
			 const struct pcap_pkthdr *h,
			 const u_char *p) {


}

/* ***************************************************** */

void extcap_capture() {
  ebpfRetCode rc;
  void *ebpf;
  u_int num = 0;
  u_int64_t bytes_written = 0;
  int err;
  u_int8_t success;
  pcap_t *pd = NULL;
  int promisc = 1;
  int snaplen = 1500;
  char errbuf[PCAP_ERRBUF_SIZE];
   
  if(debug) printf("[DEBUG][%s:%u] Capturing [ifname: %s][fifo: %s]\n",
		   __FILE__, __LINE__,
		   extcap_selected_interface ? extcap_selected_interface : "<NULL>",
		   extcap_capture_fifo ? extcap_capture_fifo : "<NULL>");
  
  ebpf = init_ebpf_flow(NULL, ebpf_process_event, &rc, 0xFFFF);

  if(ebpf == NULL) {
    fprintf(stderr, "Unable to initialize libebpfflow\n");
    return;
  }

  if((fp = fopen(extcap_capture_fifo, "wb")) == NULL) {
    fprintf(stderr, "Unable to create file %s", extcap_capture_fifo);
    return;
  }

  if(!libpcap_write_file_header(fp,
				extcap_selected_interface ? DLT_EN10MB : 0 /* DLT_NULL */,
				sizeof(eBPFevent), FALSE, &bytes_written, &err)) {
    fprintf(stderr, "Unable to write file %s header", extcap_capture_fifo);
    return;
  }

  if((signal(SIGINT, sigproc) == SIG_ERR)
     || (signal(SIGTERM, sigproc) == SIG_ERR)
     || (signal(SIGQUIT, sigproc) == SIG_ERR)) {
    fprintf(stderr, "Unable to install SIGINT/SIGTERM signal handler");
    return;
  }

  if(extcap_selected_interface) {
    if((pd = pcap_open_live(extcap_selected_interface,
			    snaplen, promisc, 1, errbuf)) == NULL) {
      printf("pcap_open_live: %s\n", errbuf);
      return;
    }
    
    while(1) {
      if(pcap_dispatch(pd, 1, pcap_processs_packet, NULL) < 0) break;
      ebpf_poll_event(ebpf, 1);
    }
    
    pcap_close(pd);
  } else {
    /* eBPF-only capture */
    
    while(1) {
      /* fprintf(stderr, "%u\n", ++num); */
      ebpf_poll_event(ebpf, 10);
    }
  }
  
  term_ebpf_flow(ebpf);

  fclose(fp);
}

/* ***************************************************** */

int extcap_print_help() {
  printf("Wireshark extcap eBPF plugin by ntop\n");

  printf("\nSupported command line options:\n");
  printf("--extcap-interfaces\n");
  printf("--extcap-version\n");
  printf("--extcap-dlts\n");
  printf("--extcap-interface <name>\n");
  printf("--extcap-config\n");
  printf("--capture\n");
  printf("--fifo <name>\n");
  printf("--debug\n");
  printf("--name <name>\n");
  printf("--custom-name <name>\n");
  printf("--help\n");

  return(0);
}

/* ***************************************************** */

int main(int argc, char *argv[]) {
  int option_idx = 0, result;
  time_t epoch;
  char date_str[EBPFDUMP_MAX_DATE_LEN];
  struct tm* tm_info;

#if 0
  /* test code */
  if(0) {
    eBPFevent x;

    printf("%d\n", offsetof(eBPFevent, proc));
    printf("%d\n", offsetof(eBPFevent, father));

    return(0);
  }
#endif

  if(argc == 1) {
    extcap_print_help();
    return EXIT_SUCCESS;
  }

  u_int defer_dlts = 0, defer_config = 0, defer_capture = 0;
  while ((result = getopt_long(argc, argv, "h", longopts, &option_idx)) != -1) {
    // fprintf(stderr, "OPT: '%c' VAL: '%s' \n", result, optarg != NULL ? optarg : "");

    switch(result) {
      /* mandatory extcap options */
    case EXTCAP_OPT_DEBUG:
      debug = 1;
      break;
    case EXTCAP_OPT_LIST_INTERFACES:
      extcap_version();
      extcap_list_interfaces();
      defer_dlts = defer_config = defer_capture = 0;
      break;
    case EXTCAP_OPT_VERSION:
      extcap_version();
      defer_dlts = defer_config = defer_capture = 0;
      break;
    case EXTCAP_OPT_LIST_DLTS:
      defer_dlts = 1; defer_config = defer_capture = 0;
      break;
    case EXTCAP_OPT_INTERFACE:
      extcap_selected_interface = strndup(optarg, EBPFDUMP_MAX_NAME_LEN);
      break;
    case EXTCAP_OPT_CONFIG:
      defer_config = 1; defer_dlts = defer_capture = 0;
      break;
    case EXTCAP_OPT_CAPTURE:
      defer_capture = 1; defer_dlts = defer_config = 0;
      break;
      break;
    case EXTCAP_OPT_FIFO:
      extcap_capture_fifo = strdup(optarg);
      break;

      /* custom ebpfdump options */
    case EBPFDUMP_OPT_HELP:
      extcap_print_help();
      return EXIT_SUCCESS;
      break;
    }
  }

  if(defer_dlts) extcap_dlts();
  else if(defer_config) extcap_config();
  else if(defer_capture) extcap_capture();

  if(extcap_selected_interface)   free(extcap_selected_interface);
  if(extcap_capture_fifo)         free(extcap_capture_fifo);

  return EXIT_SUCCESS;
}
