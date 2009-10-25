#include "globals.h"
#include "api.h"

#define ISREADY					  \
  if (!xbee_ready) {				  \
    printf("XBee: Run xbee_setup() first!...\n"); \
    exit(1);					  \
  }

/* ready flag.
   needs to be set to -1 so that the listen thread can begin.
   then 1 so that functions can be used (after setup of course...) */
int xbee_ready = 0;

/* ################################################################# */
/* ### Memory Handling ############################################# */
/* ################################################################# */

/* malloc wrapper function */
void *Xmalloc(size_t size) {
  void *t;
  t = malloc(size);
  if (!t) {
    /* uhoh... thats pretty bad... */
    perror("xbee:malloc()");
    exit(1);
  }
  return t;
}

/* calloc wrapper function */
void *Xcalloc(size_t size) {
  void *t;
  t = calloc(1, size);
  if (!t) {
    /* uhoh... thats pretty bad... */
    perror("xbee:calloc()");
    exit(1);
  }
  return t;
}

/* realloc wrapper function */
void *Xrealloc(void *ptr, size_t size) {
  void *t;
  t = realloc(ptr,size);
  if (!t) {
    /* uhoh... thats pretty bad... */
    perror("xbee:realloc()");
    exit(1);
  }
  return t;
}

/* free wrapper function (uses the Xfree macro and sets the pointer to NULL after freeing it) */
void Xfree2(void **ptr) {
  free(*ptr);
  *ptr = NULL;
}

/* ################################################################# */
/* ### XBee Functions ############################################## */
/* ################################################################# */

/* #################################################################
   xbee_setup
   opens xbee serial port & creates xbee read thread
   the xbee must be configured for API mode 2
   THIS MUST BE CALLED BEFORE ANY OTHER XBEE FUNCTION */
int xbee_setup(char *path, int baudrate) {
  t_info info;
  struct termios tc;
  speed_t chosenbaud;

  /* select the baud rate */
  switch (baudrate) {
    case 1200:  chosenbaud = B1200;   break;
    case 2400:  chosenbaud = B2400;   break;
    case 4800:  chosenbaud = B4800;   break;
    case 9600:  chosenbaud = B9600;   break;
    case 19200: chosenbaud = B19200;  break;
    case 38400: chosenbaud = B38400;  break;
    case 57600: chosenbaud = B57600;  break;
    case 115200:chosenbaud = B115200; break;
    default:
      printf("XBee: Unknown or incompatiable baud rate specified... (%d)\n",baudrate);
      return -1;
  };

  /* setup the connection mutex */
  xbee.conlist = NULL;
  if (pthread_mutex_init(&xbee.conmutex,NULL)) {
    perror("xbee_setup():pthread_mutex_init(conmutex)");
    return -1;
  }

  /* setup the packet mutex */
  xbee.pktlist = NULL;
  if (pthread_mutex_init(&xbee.pktmutex,NULL)) {
    perror("xbee_setup():pthread_mutex_init(pktmutex)");
    return -1;
  }

  /* setup the send mutex */
  if (pthread_mutex_init(&xbee.sendmutex,NULL)) {
    perror("xbee_setup():pthread_mutex_init(sendmutex)");
    return -1;
  }

  /* take a copy of the XBee device path */
  if ((xbee.path = malloc(sizeof(char) * (strlen(path) + 1))) == NULL) {
    perror("xbee_setup():malloc(path)");
    return -1;
  }
  strcpy(xbee.path,path);

  /* open the serial port as a file descriptor */
  if ((xbee.ttyfd = open(path,O_RDWR | O_NOCTTY | O_NONBLOCK)) == -1) {
    perror("xbee_setup():open()");
    Xfree(xbee.path);
    xbee.ttyfd = -1;
    xbee.tty = NULL;
    return -1;
  }
  /* setup the baud rate and other io attributes */
  tcgetattr(xbee.ttyfd, &tc);
  cfsetispeed(&tc, chosenbaud);       /* set input baud rate to 57600 */
  cfsetospeed(&tc, chosenbaud);       /* set output baud rate to 57600 */
  /* input flags */
  tc.c_iflag |= IGNBRK;            /* enable ignoring break */
  tc.c_iflag &= ~(IGNPAR | PARMRK);/* disable parity checks */
  tc.c_iflag &= ~INPCK;            /* disable parity checking */
  tc.c_iflag &= ~ISTRIP;           /* disable stripping 8th bit */
  tc.c_iflag &= ~(INLCR | ICRNL);  /* disable translating NL <-> CR */
  tc.c_iflag &= ~IGNCR;            /* disable ignoring CR */
  tc.c_iflag &= ~(IXON | IXOFF);   /* disable XON/XOFF flow control */
  /* output flags */
  tc.c_oflag &= ~OPOST;            /* disable output processing */
  tc.c_oflag &= ~(ONLCR | OCRNL);  /* disable translating NL <-> CR */
  tc.c_oflag &= ~OFILL;            /* disable fill characters */
  /* control flags */
  tc.c_cflag |= CREAD;             /* enable reciever */
  tc.c_cflag &= ~PARENB;           /* disable parity */
  tc.c_cflag &= ~CSTOPB;           /* disable 2 stop bits */
  tc.c_cflag &= ~CSIZE;            /* remove size flag... */
  tc.c_cflag |= CS8;               /* ...enable 8 bit characters */
  tc.c_cflag |= HUPCL;             /* enable lower control lines on close - hang up */
  /* local flags */
  tc.c_lflag &= ~ISIG;             /* disable generating signals */
  tc.c_lflag &= ~ICANON;           /* disable canonical mode - line by line */
  tc.c_lflag &= ~ECHO;             /* disable echoing characters */
  tc.c_lflag &= ~NOFLSH;           /* disable flushing on SIGINT */
  tc.c_lflag &= ~IEXTEN;           /* disable input processing */
  tcsetattr(xbee.ttyfd, TCSANOW, &tc);

  /* open the serial port as a FILE* */
  if ((xbee.tty = fdopen(xbee.ttyfd,"r+")) == NULL) {
    perror("xbee_setup():fdopen()");
    Xfree(xbee.path);
    close(xbee.ttyfd);
    xbee.ttyfd = -1;
    xbee.tty = NULL;
    return -1;
  }

  /* flush the serial port */
  fflush(xbee.tty);

  /* allow the listen thread to start */
  xbee_ready = -1;

  /* can start xbee_listen thread now */
  if (pthread_create(&xbee.listent,NULL,(void *(*)(void *))xbee_listen,(void *)&info) != 0) {
    perror("xbee_setup():pthread_create()");
    Xfree(xbee.path);
    fclose(xbee.tty);
    close(xbee.ttyfd);
    xbee.ttyfd = -1;
    xbee.tty = NULL;
    return -1;
  }

  /* allow other functions to be used! */
  xbee_ready = 1;
  return 0;
}

/* #################################################################
   xbee_con
   produces a connection to the specified device and frameID
   if a connection had already been made, then this connection will be returned */
xbee_con *xbee_newcon(unsigned char frameID, xbee_types type, ...) {
  xbee_con *con, *ocon;
  unsigned char tAddr[8];
  va_list ap;
  int t;
#ifdef DEBUG
  int i;
#endif

  ISREADY;

  if (!type || type == xbee_unknown) type = xbee_localAT; /* default to local AT */
  else if (type == xbee_remoteAT) type = xbee_64bitRemoteAT; /* if remote AT, default to 64bit */

  va_start(ap,type);
  /* if: 64 bit address expected (2 ints) */
  if ((type == xbee_64bitRemoteAT) ||
      (type == xbee_64bitData) ||
      (type == xbee_64bitIO)) {
    t = va_arg(ap, int);
    tAddr[0] = (t >> 24) & 0xFF;
    tAddr[1] = (t >> 16) & 0xFF;
    tAddr[2] = (t >>  8) & 0xFF;
    tAddr[3] = (t      ) & 0xFF;
    t = va_arg(ap, int);
    tAddr[4] = (t >> 24) & 0xFF;
    tAddr[5] = (t >> 16) & 0xFF;
    tAddr[6] = (t >>  8) & 0xFF;
    tAddr[7] = (t      ) & 0xFF;

  /* if: 16 bit address expected (1 int) */
  } else if ((type == xbee_16bitRemoteAT) ||
	     (type == xbee_16bitData) ||
	     (type == xbee_16bitIO)) {
    t = va_arg(ap, int);
    tAddr[0] = (t >>  8) & 0xFF;
    tAddr[1] = (t      ) & 0xFF;
    tAddr[2] = 0;
    tAddr[3] = 0;
    tAddr[4] = 0;
    tAddr[5] = 0;
    tAddr[6] = 0;
    tAddr[7] = 0;

  /* otherwise clear the address */
  } else {
    memset(tAddr,0,8);
  }
  va_end(ap);

  /* lock the connection mutex */
  pthread_mutex_lock(&xbee.conmutex);

  /* are there any connections? */
  if (xbee.conlist) {
    con = xbee.conlist;
    while (con) {
      /* if: after a modemStatus, and the types match! */
      if ((type == xbee_modemStatus) &&
	  (con->type == type)) {
	pthread_mutex_unlock(&xbee.conmutex);
	return con;

      /* if: after a txStatus and frameIDs match! */
      } else if ((type == xbee_txStatus) &&
		 (con->type == type) &&
		 (frameID == con->frameID)) {
	pthread_mutex_unlock(&xbee.conmutex);
	return con;

      /* if: after a localAT, and the frameIDs match! */
      } else if ((type == xbee_localAT) &&
		 (con->type == type) &&
		 (frameID == con->frameID)) {
	pthread_mutex_unlock(&xbee.conmutex);
	return con;

      /* if: connection types match, the frameIDs match, and the addresses match! */
      } else if ((type == con->type) &&
		 (frameID == con->frameID) &&
		 (!memcmp(tAddr,con->tAddr,8))) {
	pthread_mutex_unlock(&xbee.conmutex);
	return con;
      }

      /* if there are more, move along, dont want to loose that last item! */
      if (con->next == NULL) break;
      con = con->next;
    }

    /* keep hold of the last connection... we will need to link it up later */
    ocon = con;
  }

  /* create a new connection and set its attributes */
  con = Xcalloc(sizeof(xbee_con));
  con->type = type;
  /* is it a 64bit connection? */
  if ((type == xbee_64bitRemoteAT) ||
      (type == xbee_64bitData) ||
      (type == xbee_64bitIO)) {
    con->tAddr64 = TRUE;
  }
  con->atQueue = 0; /* queue AT commands? */
  con->txDisableACK = 0; /* disable ACKs? */
  con->txBroadcast = 0; /* broadcast? */
  con->frameID = frameID;
  memcpy(con->tAddr,tAddr,8); /* copy in the remote address */

#ifdef DEBUG
  switch(type) {
    case xbee_localAT:
      printf("XBee: New local AT connection!\n");
      break;
    case xbee_16bitRemoteAT:
    case xbee_64bitRemoteAT:
      printf("XBee: New %d-bit remote AT connection! (to: ",(con->tAddr64?64:16));
      for (i=0;i<(con->tAddr64?8:2);i++) {
        printf((i?":%02X":"%02X"),tAddr[i]);
      }
      printf(")\n");
      break;
    case xbee_16bitData:
    case xbee_64bitData:
      printf("XBee: New %d-bit data connection! (to: ",(con->tAddr64?64:16));
      for (i=0;i<(con->tAddr64?8:2);i++) {
        printf((i?":%02X":"%02X"),tAddr[i]);
      }
      printf(")\n");
      break;
    case xbee_16bitIO:
    case xbee_64bitIO:
      printf("XBee: New %d-bit IO connection! (to: ",(con->tAddr64?64:16));
      for (i=0;i<(con->tAddr64?8:2);i++) {
        printf((i?":%02X":"%02X"),tAddr[i]);
      }
      printf(")\n");
      break;
    case xbee_txStatus:
      printf("XBee: New Tx status connection!\n");
      break;
    case xbee_modemStatus:
      printf("XBee: New modem status connection!\n");
      break;
    case xbee_unknown:
    default:
      printf("XBee: New unknown connection!\n");
  }
#endif

  /* make it the last in the list */
  con->next = NULL;
  /* add it to the list */
  if (xbee.conlist) {
    ocon->next = con;
  } else {
    xbee.conlist = con;
  }

  /* unlock the mutex */
  pthread_mutex_unlock(&xbee.conmutex);
  return con;
}

/* #################################################################
   xbee_senddata
   send the specified data to the provided connection */
int xbee_senddata(xbee_con *con, char *format, ...) {
  int retval;
  va_list ap;

  ISREADY;

  /* xbee_vsenddata() wants a va_list... */
  va_start(ap, format);
  /* hand it over :) */
  retval = xbee_vsenddata(con,format,ap);
  va_end(ap);
  return retval;
}

int xbee_vsenddata(xbee_con *con, char *format, va_list ap) {
  t_data *pkt;
  int i, length;
  unsigned char buf[128]; /* max payload is 100 bytes... plus a bit for the headers etc... */
  unsigned char data[128]; /* ditto */

  ISREADY;

  if (!con) return -1;
  if (con->type == xbee_unknown) return -1;

  /* lock the send mutex */
  pthread_mutex_lock(&xbee.sendmutex);

  /* make up the data and keep the length, its possible there are nulls in there */
  length = vsnprintf((char *)data,128,format,ap);

#ifdef DEBUG
  printf("XBee: --== TX Packet ============--\n");
  printf("XBee: Length: %d\n",length);
  for (i=0;i<length;i++) {
    printf("XBee: %3d | 0x%02X ",i,data[i]);
    if ((data[i] > 32) && (data[i] < 127)) printf("'%c'\n",data[i]); else printf(" _\n");
  }
#endif

  /* ########################################## */
  /* if: local AT mode */
  if (con->type == xbee_localAT) {
    /* AT commands are 2 chars long (plus optional parameter) */
    if (length < 2) return -1;

    /* use the command? */
    buf[0] = ((!con->atQueue)?0x08:0x09);
    buf[1] = con->frameID;

    /* copy in the data */
    for (i=0;i<length;i++) {
      buf[i+2] = data[i];
    }

    /* setup the packet */
    pkt = xbee_make_pkt(buf,i+2);
    /* send it on */
    xbee_send_pkt(pkt);

    /* unlock the mutex */
    pthread_mutex_unlock(&xbee.sendmutex);
    return 1;
  /* ########################################## */
  /* if: remote AT mode */
  } else if ((con->type == xbee_16bitRemoteAT) ||
	     (con->type == xbee_64bitRemoteAT)) {
    if (length < 2) return -1; /* at commands are 2 chars long (plus optional parameter) */
    buf[0] = 0x17;
    buf[1] = con->frameID;

    /* copy in the relevant address */
    if (con->tAddr64) {
      memcpy(&buf[2],con->tAddr,8);
      buf[10] = 0xFF;
      buf[11] = 0xFE;
    } else {
      memset(&buf[2],0,8);
      memcpy(&buf[10],con->tAddr,2);
    }
    /* queue the command? */
    buf[12] = ((!con->atQueue)?0x02:0x00);

    /* copy in the data */
    for (i=0;i<length;i++) {
      buf[i+13] = data[i];
    }

    /* setup the packet */
    pkt = xbee_make_pkt(buf,i+13);
    /* send it on */
    xbee_send_pkt(pkt);

    /* unlock the mutex */
    pthread_mutex_unlock(&xbee.sendmutex);
    return 1;
  /* ########################################## */
  /* if: 64bit Data */
  } else if (con->type == xbee_64bitData) {
    buf[0] = 0x00;
    buf[1] = con->frameID;

    /* copy in the address */
    memcpy(&buf[2],con->tAddr,8);

    /* broadcast? */
    buf[10] = ((con->txDisableACK)?0x01:0x00) | ((con->txBroadcast)?0x04:0x00);

    /* copy in the data */
    for (i=0;i<length;i++) {
      buf[i+11] = data[i];
    }

    /* setup the packet */
    pkt = xbee_make_pkt(buf,i+11);
    /* send it on */
    xbee_send_pkt(pkt);

    /* unlock the mutex */
    pthread_mutex_unlock(&xbee.sendmutex);
    return 1;
  /* ########################################## */
  /* if: 16bit Data */
  } else if (con->type == xbee_16bitData) {
    buf[0] = 0x01;
    buf[1] = con->frameID;

    /* copy in the address */
    memcpy(&buf[2],con->tAddr,2);

    /* broadcast? */
    buf[4] = ((con->txDisableACK)?0x01:0x00) | ((con->txBroadcast)?0x04:0x00);

    /* copy in the data */
    for (i=0;i<length;i++) {
      buf[i+5] = data[i];
    }

    /* setup the packet */
    pkt = xbee_make_pkt(buf,i+5);
    /* send it on */
    xbee_send_pkt(pkt);

    /* unlock the mutex */
    pthread_mutex_unlock(&xbee.sendmutex);
    return 1;
  /* ########################################## */
  /* if: I/O */
  } else if ((con->type == xbee_64bitIO) ||
	     (con->type == xbee_16bitIO)) {
    /* not currently implemented... */
    printf("******* TODO ********\n");
  }

  /* unlock the mutex */
  pthread_mutex_unlock(&xbee.sendmutex);
  return 0;
}

/* #################################################################
   xbee_getpacket
   retrieves the next packet destined for the given connection
   once the packet has been retrieved, it is removed for the list! */
xbee_pkt *xbee_getpacket(xbee_con *con) {
  xbee_pkt *l, *p, *q;
#ifdef DEBUG
  int c;
  printf("XBee: --== Get Packet ==========--\n");
#endif

  /* lock the packet mutex */
  pthread_mutex_lock(&xbee.pktmutex);

  /* if: there are no packets */
  if ((p = xbee.pktlist) == NULL) {
    pthread_mutex_unlock(&xbee.pktmutex);
#ifdef DEBUG
    printf("XBee: No packets avaliable...\n");
#endif
    return NULL;
  }

  l = NULL;
  q = NULL;
  /* get the first avaliable packet for this socket */
  do {
    /* if: the connection type matches the packet type OR
       the connection is 16/64bit remote AT, and the packet is a remote AT response */
    if ((p->type == con->type) || /* -- */
	((p->type == xbee_remoteAT) && /* -- */
	 ((con->type == xbee_16bitRemoteAT) ||
	  (con->type == xbee_64bitRemoteAT)))) {

      /* if: the packet is modem status OR
	 the packet is tx status or AT data and the frame IDs match OR
	 the addresses match */
      if ((p->type == xbee_modemStatus) ||
	  (((p->type == xbee_txStatus) ||
	    (p->type == xbee_localAT) ||
	    (p->type == xbee_remoteAT)) &&
	   (con->frameID == p->frameID)) ||
	  (!memcmp(con->tAddr,p->Addr64,8))) {
	q = p;
	break;
      }
    }

    /* move on */
    l = p;
    p = p->next;
  } while (p);

  /* if: no packet was found */
  if (!q) {
    pthread_mutex_unlock(&xbee.pktmutex);
#ifdef DEBUG
    printf("XBee: No packets avaliable (for connection)...\n");
#endif
    return NULL;
  }

  /* if it was the first packet */
  if (!l) {
    /* move the chain along */
    xbee.pktlist = p->next;
  } else {
    /* otherwise relink the list */
    l->next = p->next;
  }

#ifdef DEBUG
  printf("XBee: Got a packet\n");
  for (p = xbee.pktlist,c = 0;p;c++,p = p->next);
  printf("XBee: Packets left: %d\n",c);
#endif

  /* unlock the packet mutex */
  pthread_mutex_unlock(&xbee.pktmutex);

  /* and return the packet (must be freed by caller!) */
  return q;
}

/* #################################################################
   xbee_listen - INTERNAL
   the xbee xbee_listen thread
   reads data from the xbee and puts it into a linked list to keep the xbee buffers free */
void xbee_listen(t_info *info) {
  unsigned char c, t, d[128];
  unsigned int l, i, chksum, o;
#ifdef DEBUG
  int j;
#endif
  xbee_pkt *p, *q, *po;

  /* just falls out if the proper 'go-ahead' isn't given */
  if (xbee_ready != -1) return;

  /* do this forever :) */
  while(1) {
    /* wait for a valid start byte */
    if (xbee_getRawByte() != 0x7E) continue;

#ifdef DEBUG
    printf("XBee: --== RX Packet ===========--\nXBee: Got a packet!...\n");
#endif

    /* get the length */
    l = xbee_getByte() << 8;
    l += xbee_getByte();

    /* check it is a valid length... */
    if (!l) {
#ifdef DEBUG
      printf("XBee: Recived zero length packet!\n");
#endif
      continue;
    }
    if (l > 100) {
#ifdef DEBUG
      printf("XBee: Recived oversized packet! Length: %d\n",l - 1);
#endif
      continue;
    }

#ifdef DEBUG
    printf("XBee: Length: %d\n",l - 1);
#endif

    /* get the packet type */
    t = xbee_getByte();

    /* start the checksum */
    chksum = t;

    /* suck in all the data */
    for (i = 0; l > 1 && i < 128; l--, i++) {
      /* get an unescaped byte */
      c = xbee_getByte();
      d[i] = c;
      chksum += c;
#ifdef DEBUG
      printf("XBee: %3d | '%c' | 0x%02X ",(((c < '~') && (c > ' '))?c:'.'),i,c);
      if ((c > 32) && (c < 127)) printf("'%c'\n",c); else printf(" _\n");
#endif
    }
    i--; /* it went up too many times!... */

    /* add the checksum */
    chksum += xbee_getByte();

    /* check if the whole packet was recieved, or something else occured... unlikely... */
    if (l>1) {
#ifdef DEBUG
      printf("XBee: Didn't get whole packet... :(\n");
#endif
      continue;
    }

    /* check the checksum */
    if ((chksum & 0xFF) != 0xFF) {
#ifdef DEBUG
      printf("XBee: Invalid Checksum: 0x%02X\n",c,chksum);
#endif
      continue;
    }

    /* make a new packet */
    po = p = Xcalloc(sizeof(xbee_pkt));
    q = NULL;
    p->datalen = 0;

    /* ########################################## */
    /* if: modem status */
    if (t == 0x8A) {
#ifdef DEBUG
      printf("XBee: Packet type: Modem Status (0x8A)\n");
      printf("XBee: ");
      switch (d[0]) {
      case 0x00: printf("Hardware reset"); break;
      case 0x01: printf("Watchdog timer reset"); break;
      case 0x02: printf("Associated"); break;
      case 0x03: printf("Disassociated"); break;
      case 0x04: printf("Synchronization lost"); break;
      case 0x05: printf("Coordinator realignment"); break;
      case 0x06: printf("Coordinator started"); break;
      }
      printf("...\n");
#endif
      p->type = xbee_modemStatus;

      p->sAddr64 = FALSE;
      p->dataPkt = FALSE;
      p->txStatusPkt = FALSE;
      p->modemStatusPkt = TRUE;
      p->remoteATPkt = FALSE;
      p->IOPkt = FALSE;

      /* modem status can only ever give 1 'data' byte */
      p->datalen = 1;
      p->data[0] = d[0];

    /* ########################################## */
    /* if: local AT response */
    } else if (t == 0x88) {
#ifdef DEBUG
      printf("XBee: Packet type: Local AT Response (0x88)\n");
      printf("XBee: FrameID: 0x%02X\n",d[0]);
      printf("XBee: AT Command: %c%c\n",d[1],d[2]);
      if (d[3] == 0) printf("XBee: Status: OK\n");
      else if (d[3] == 1) printf("XBee: Status: Error\n");
      else if (d[3] == 2) printf("XBee: Status: Invalid Command\n");
      else if (d[3] == 3) printf("XBee: Status: Invalid Parameter\n");
#endif
      p->type = xbee_localAT;

      p->sAddr64 = FALSE;
      p->dataPkt = FALSE;
      p->txStatusPkt = FALSE;
      p->modemStatusPkt = FALSE;
      p->remoteATPkt = FALSE;
      p->IOPkt = FALSE;

      p->frameID = d[0];
      p->atCmd[0] = d[1];
      p->atCmd[1] = d[2];

      p->status = d[3];

      /* copy in the data */
      p->datalen = i-3;
      for (;i>3;i--) p->data[i-4] = d[i];

    /* ########################################## */
    /* if: remote AT response */
    } else if (t == 0x97) {
#ifdef DEBUG
      printf("XBee: Packet type: Remote AT Response (0x97)\n");
      printf("XBee: FrameID: 0x%02X\n",d[0]);
      printf("XBee: 64-bit Address: ");
      for (j=0;j<8;j++) {
	printf((j?":%02X":"%02X"),d[1+j]);
      }
      printf("\n");
      printf("XBee: 16-bit Address: ");
      for (j=0;j<2;j++) {
	printf((j?":%02X":"%02X"),d[9+j]);
      }
      printf("\n");
      printf("XBee: AT Command: %c%c\n",d[11],d[12]);
      if (d[13] == 0) printf("XBee: Status: OK\n");
      else if (d[13] == 1) printf("XBee: Status: Error\n");
      else if (d[13] == 2) printf("XBee: Status: Invalid Command\n");
      else if (d[13] == 3) printf("XBee: Status: Invalid Parameter\n");
      else if (d[13] == 4) printf("XBee: Status: No Response\n");
#endif
      p->type = xbee_remoteAT;

      p->sAddr64 = FALSE;
      p->dataPkt = FALSE;
      p->txStatusPkt = FALSE;
      p->modemStatusPkt = FALSE;
      p->remoteATPkt = TRUE;
      p->IOPkt = FALSE;

      p->frameID = d[0];

      p->Addr64[0] = d[1];
      p->Addr64[1] = d[2];
      p->Addr64[2] = d[3];
      p->Addr64[3] = d[4];
      p->Addr64[4] = d[5];
      p->Addr64[5] = d[6];
      p->Addr64[6] = d[7];
      p->Addr64[7] = d[8];

      p->Addr16[0] = d[9];
      p->Addr16[1] = d[10];

      p->atCmd[0] = d[11];
      p->atCmd[1] = d[12];

      p->status = d[13];

      /* copy in the data */
      p->datalen = i-13;
      for (;i>13;i--) p->data[i-14] = d[i];

    /* ########################################## */
    /* if: TX status */
    } else if (t == 0x89) {
#ifdef DEBUG
      printf("XBee: Packet type: TX Status Report (0x89)\n");
      printf("XBee: FrameID: 0x%02X\n",d[0]);
      if (d[1] == 0) printf("XBee: Status: Success\n");
      else if (d[1] == 1) printf("XBee: Status: No ACK\n");
      else if (d[1] == 2) printf("XBee: Status: CCA Failure\n");
      else if (d[1] == 3) printf("XBee: Status: Purged\n");
#endif
      p->type = xbee_txStatus;

      p->sAddr64 = FALSE;
      p->dataPkt = FALSE;
      p->txStatusPkt = TRUE;
      p->modemStatusPkt = FALSE;
      p->remoteATPkt = FALSE;
      p->IOPkt = FALSE;

      p->frameID = d[0];

      p->status = d[1];

      /* never returns data */
	p->datalen = 0;

    /* ########################################## */
    /* if: 64bit address recieve */
    } else if (t == 0x80) {
#ifdef DEBUG
      printf("XBee: Packet type: 64-bit RX Data (0x80)\n");
      printf("XBee: 64-bit Address: ");
      for (j=0;j<8;j++) {
	printf((j?":%02X":"%02X"),d[j]);
      }
      printf("\n");
      printf("XBee: RSSI: -%ddB\n",d[8]);
      if (d[9] & 0x02) printf("XBee: Options: Address Broadcast\n");
      if (d[9] & 0x03) printf("XBee: Options: PAN Broadcast\n");
#endif
      p->type = xbee_64bitData;

      p->sAddr64 = TRUE;
      p->dataPkt = TRUE;
      p->txStatusPkt = FALSE;
      p->modemStatusPkt = FALSE;
      p->remoteATPkt = FALSE;
      p->IOPkt = FALSE;

      p->Addr64[0] = d[0];
      p->Addr64[1] = d[1];
      p->Addr64[2] = d[2];
      p->Addr64[3] = d[3];
      p->Addr64[4] = d[4];
      p->Addr64[5] = d[5];
      p->Addr64[6] = d[6];
      p->Addr64[7] = d[7];

      /* save the RSSI / signal strength
	 this can be used with printf as:
	 printf("-%ddB\n",p->RSSI); */
      p->RSSI = d[8];

      p->status = d[9];

      /* copy in the data */
      p->datalen = i-9;
      for (;i>9;i--) p->data[i-10] = d[i];

    /* ########################################## */
    /* if: 16bit address recieve */
    } else if (t == 0x81) {
#ifdef DEBUG
      printf("XBee: Packet type: 16-bit RX Data (0x81)\n");
      printf("XBee: 16-bit Address: ");
      for (j=0;j<2;j++) {
	printf((j?":%02X":"%02X"),d[j]);
      }
      printf("\n");
      printf("XBee: RSSI: -%ddB\n",d[2]);
      if (d[3] & 0x02) printf("XBee: Options: Address Broadcast\n");
      if (d[3] & 0x03) printf("XBee: Options: PAN Broadcast\n");
#endif
      p->type = xbee_16bitData;

      p->sAddr64 = FALSE;
      p->dataPkt = TRUE;
      p->txStatusPkt = FALSE;
      p->modemStatusPkt = FALSE;
      p->remoteATPkt = FALSE;
      p->IOPkt = FALSE;

      p->Addr16[0] = d[0];
      p->Addr16[1] = d[1];

      /* save the RSSI / signal strength
	 this can be used with printf as:
	 printf("-%ddB\n",p->RSSI); */
      p->RSSI = d[2];

      p->status = d[3];

      /* copy in the data */
      p->datalen = i-3;
      for (;i>3;i--) p->data[i-4] = d[i];

    /* ########################################## */
    /* if: 64bit I/O recieve */
    } else if (t == 0x82) {
#ifdef DEBUG
      printf("XBee: Packet type: 64-bit RX I/O Data (0x82)\n");
      printf("XBee: 64-bit Address: ");
      for (j=0;j<8;j++) {
	printf((j?":%02X":"%02X"),d[j]);
      }
      printf("\n");
      printf("XBee: RSSI: -%ddB\n",d[8]);
      if (d[9] & 0x02) printf("XBee: Options: Address Broadcast\n");
      if (d[9] & 0x02) printf("XBee: Options: PAN Broadcast\n");
      printf("XBee: Samples: %d\n",d[10]);
#endif
      i = 13;

      /* each sample is split into its own packet here, for simplicity */
      for (o=d[10];o>0;o--) {
#ifdef DEBUG
	printf("XBee: --- Sample %3d -------------\n",o-d[10]+1);
#endif
	/* if we arent still using the origional packet */
	if (o<d[10]) {
	  /* make a new one and link it up! */
	  q = Xcalloc(sizeof(xbee_pkt));
	  p->next = q;
	  p = q;
	}

	/* never returns data */
	p->datalen = 0;

	p->type = xbee_64bitIO;

	p->sAddr64 = TRUE;
	p->dataPkt = FALSE;
	p->txStatusPkt = FALSE;
	p->modemStatusPkt = FALSE;
	p->remoteATPkt = FALSE;
	p->IOPkt = TRUE;

	p->Addr64[0] = d[0];
	p->Addr64[1] = d[1];
	p->Addr64[2] = d[2];
	p->Addr64[3] = d[3];
	p->Addr64[4] = d[4];
	p->Addr64[5] = d[5];
	p->Addr64[6] = d[6];
	p->Addr64[7] = d[7];

	/* save the RSSI / signal strength
	   this can be used with printf as:
	   printf("-%ddB\n",p->RSSI); */
	p->RSSI = d[8];

	p->status = d[9];

	/* copy in the I/O data mask */
	p->IOmask = (((d[11]<<8) | d[12]) & 0x7FFF);

	/* copy in the digital I/O data */
	p->IOdata = (((d[i]<<8) | d[i+1]) & 0x01FF);

	/* advance over the digital data, if its there */
	i += (((d[11]&0x01)||(d[12]))?2:0);

	/* copy in the analog I/O data */
	if (d[11]&0x02) {p->IOanalog[0] = (((d[i]<<8) | d[i+1]) & 0x03FF);i+=2;}
	if (d[11]&0x04) {p->IOanalog[1] = (((d[i]<<8) | d[i+1]) & 0x03FF);i+=2;}
	if (d[11]&0x08) {p->IOanalog[2] = (((d[i]<<8) | d[i+1]) & 0x03FF);i+=2;}
	if (d[11]&0x10) {p->IOanalog[3] = (((d[i]<<8) | d[i+1]) & 0x03FF);i+=2;}
	if (d[11]&0x20) {p->IOanalog[4] = (((d[i]<<8) | d[i+1]) & 0x03FF);i+=2;}
	if (d[11]&0x40) {p->IOanalog[5] = (((d[i]<<8) | d[i+1]) & 0x03FF);i+=2;}
#ifdef DEBUG
	if (p->IOmask & 0x0001) printf("XBee: Digital 0: %c\n",((p->IOdata & 0x0001)?'1':'0'));
	if (p->IOmask & 0x0002) printf("XBee: Digital 1: %c\n",((p->IOdata & 0x0002)?'1':'0'));
	if (p->IOmask & 0x0004) printf("XBee: Digital 2: %c\n",((p->IOdata & 0x0004)?'1':'0'));
	if (p->IOmask & 0x0008) printf("XBee: Digital 3: %c\n",((p->IOdata & 0x0008)?'1':'0'));
	if (p->IOmask & 0x0010) printf("XBee: Digital 4: %c\n",((p->IOdata & 0x0010)?'1':'0'));
	if (p->IOmask & 0x0020) printf("XBee: Digital 5: %c\n",((p->IOdata & 0x0020)?'1':'0'));
	if (p->IOmask & 0x0040) printf("XBee: Digital 6: %c\n",((p->IOdata & 0x0040)?'1':'0'));
	if (p->IOmask & 0x0080) printf("XBee: Digital 7: %c\n",((p->IOdata & 0x0080)?'1':'0'));
	if (p->IOmask & 0x0100) printf("XBee: Digital 8: %c\n",((p->IOdata & 0x0100)?'1':'0'));
	if (p->IOmask & 0x0200) printf("XBee: Analog  0: %.2fv\n",(3.3/1023)*p->IOanalog[0]);
	if (p->IOmask & 0x0400) printf("XBee: Analog  1: %.2fv\n",(3.3/1023)*p->IOanalog[1]);
	if (p->IOmask & 0x0800) printf("XBee: Analog  2: %.2fv\n",(3.3/1023)*p->IOanalog[2]);
	if (p->IOmask & 0x1000) printf("XBee: Analog  3: %.2fv\n",(3.3/1023)*p->IOanalog[3]);
	if (p->IOmask & 0x2000) printf("XBee: Analog  4: %.2fv\n",(3.3/1023)*p->IOanalog[4]);
	if (p->IOmask & 0x4000) printf("XBee: Analog  5: %.2fv\n",(3.3/1023)*p->IOanalog[5]);
#endif
      }
#ifdef DEBUG
      printf("XBee: ----------------------------\n");
#endif

    /* ########################################## */
    /* if: 16bit I/O recieve */
    } else if (t == 0x83) {
#ifdef DEBUG
      printf("XBee: Packet type: 16-bit RX I/O Data (0x83)\n");
      printf("XBee: 16-bit Address: ");
      for (j=0;j<2;j++) {
	printf((j?":%02X":"%02X"),d[j]);
      }
      printf("\n");
      printf("XBee: RSSI: -%ddB\n",d[2]);
      if (d[3] & 0x02) printf("XBee: Options: Address Broadcast\n");
      if (d[3] & 0x02) printf("XBee: Options: PAN Broadcast\n");
      printf("XBee: Samples: %d\n",d[4]);
#endif

      i = 7;

      /* each sample is split into its own packet here, for simplicity */
      for (o=d[4];o>0;o--) {
#ifdef DEBUG
	printf("XBee: --- Sample %3d -------------\n",o-d[4]+1);
#endif
	if (o<d[4]) {
	  q = Xcalloc(sizeof(xbee_pkt));
	  p->next = q;
	  p = q;
	}

	/* never returns data */
	p->datalen = 0;

	p->type = xbee_16bitIO;

	p->sAddr64 = FALSE;
	p->dataPkt = FALSE;
	p->txStatusPkt = FALSE;
	p->modemStatusPkt = FALSE;
	p->remoteATPkt = FALSE;
	p->IOPkt = TRUE;

	p->Addr16[0] = d[0];
	p->Addr16[1] = d[1];

	/* save the RSSI / signal strength
	   this can be used with printf as:
	   printf("-%ddB\n",p->RSSI); */
	p->RSSI = d[2];

	p->status = d[3];

	/* copy in the I/O data mask */
	p->IOmask = (((d[5]<<8) | d[6]) & 0x7FFF);

	/* copy in the digital I/O data */
	p->IOdata = (((d[i]<<8) | d[i+1]) & 0x01FF);

	/* advance over the digital data, if its there */
	i += (((d[5]&0x01)||(d[6]))?2:0);

	/* copy in the analog I/O data */
	if (d[5]&0x02) {p->IOanalog[0] = (((d[i]<<8) | d[i+1]) & 0x03FF);i+=2;}
	if (d[5]&0x04) {p->IOanalog[1] = (((d[i]<<8) | d[i+1]) & 0x03FF);i+=2;}
	if (d[5]&0x08) {p->IOanalog[2] = (((d[i]<<8) | d[i+1]) & 0x03FF);i+=2;}
	if (d[5]&0x10) {p->IOanalog[3] = (((d[i]<<8) | d[i+1]) & 0x03FF);i+=2;}
	if (d[5]&0x20) {p->IOanalog[4] = (((d[i]<<8) | d[i+1]) & 0x03FF);i+=2;}
	if (d[5]&0x40) {p->IOanalog[5] = (((d[i]<<8) | d[i+1]) & 0x03FF);i+=2;}
#ifdef DEBUG
	if (p->IOmask & 0x0001) printf("XBee: Digital 0: %c\n",((p->IOdata & 0x0001)?'1':'0'));
	if (p->IOmask & 0x0002) printf("XBee: Digital 1: %c\n",((p->IOdata & 0x0002)?'1':'0'));
	if (p->IOmask & 0x0004) printf("XBee: Digital 2: %c\n",((p->IOdata & 0x0004)?'1':'0'));
	if (p->IOmask & 0x0008) printf("XBee: Digital 3: %c\n",((p->IOdata & 0x0008)?'1':'0'));
	if (p->IOmask & 0x0010) printf("XBee: Digital 4: %c\n",((p->IOdata & 0x0010)?'1':'0'));
	if (p->IOmask & 0x0020) printf("XBee: Digital 5: %c\n",((p->IOdata & 0x0020)?'1':'0'));
	if (p->IOmask & 0x0040) printf("XBee: Digital 6: %c\n",((p->IOdata & 0x0040)?'1':'0'));
	if (p->IOmask & 0x0080) printf("XBee: Digital 7: %c\n",((p->IOdata & 0x0080)?'1':'0'));
	if (p->IOmask & 0x0100) printf("XBee: Digital 8: %c\n",((p->IOdata & 0x0100)?'1':'0'));
	if (p->IOmask & 0x0200) printf("XBee: Analog  0: %.2fv\n",(3.3/1023)*p->IOanalog[0]);
	if (p->IOmask & 0x0400) printf("XBee: Analog  1: %.2fv\n",(3.3/1023)*p->IOanalog[1]);
	if (p->IOmask & 0x0800) printf("XBee: Analog  2: %.2fv\n",(3.3/1023)*p->IOanalog[2]);
	if (p->IOmask & 0x1000) printf("XBee: Analog  3: %.2fv\n",(3.3/1023)*p->IOanalog[3]);
	if (p->IOmask & 0x2000) printf("XBee: Analog  4: %.2fv\n",(3.3/1023)*p->IOanalog[4]);
	if (p->IOmask & 0x4000) printf("XBee: Analog  5: %.2fv\n",(3.3/1023)*p->IOanalog[5]);
#endif
      }
#ifdef DEBUG
      printf("XBee: ----------------------------\n");
#endif

    /* ########################################## */
    /* if: Unknown */
    } else {
#ifdef DEBUG
      printf("XBee: Packet type: Unknown (0x%02X)\n",t);
#endif
      p->type = xbee_unknown;
    }
    p->next = NULL;

    /* lock the packet mutex, so we can savely add the packet to the list */
    pthread_mutex_lock(&xbee.pktmutex);
    i = 1;
    /* if: the list is empty */
    if (!xbee.pktlist) {
      /* start the list! */
      xbee.pktlist = po;
    } else {
      /* add the packet to the end */
      q = xbee.pktlist;
      while (q->next) {
	q = q->next;
	i++;
      }
      q->next = po;
    }

#ifdef DEBUG
    while (q && q->next) {
      q = q->next;
      i++;
    }
    printf("XBee: --========================--\n");
    printf("XBee: Packets: %d\n",i);
#endif

    po = p = q = NULL;

    /* unlock the packet mutex */
    pthread_mutex_unlock(&xbee.pktmutex);
  }
}

/* #################################################################
   xbee_getByte - INTERNAL
   waits for an escaped byte of data */
unsigned char xbee_getByte(void) {
  unsigned char c;

  ISREADY;

  /* take a byte */
  c = xbee_getRawByte();
  /* if its escaped, take another and un-escape */
  if (c == 0x7D) c = xbee_getRawByte() ^ 0x20;

  return (c & 0xFF);
}

/* #################################################################
   xbee_getRawByte - INTERNAL
   waits for a raw byte of data */
unsigned char xbee_getRawByte(void) {
  unsigned char c;
  fd_set fds;

  ISREADY;

  /* wait for a read to be possible */
  FD_ZERO(&fds);
  FD_SET(xbee.ttyfd,&fds);
  if (select(xbee.ttyfd+1,&fds,NULL,NULL,NULL) == -1) {
    perror("xbee:xbee_listen():xbee_getByte()");
    exit(1);
  }

  /* read 1 character
     the loop is just incase there actually isnt a byte there to be read... */
  do {
    if (read(xbee.ttyfd,&c,1) == 0) {
      usleep(10);
      continue;
    }
  } while (0);

  return (c & 0xFF);
}

/* #################################################################
   xbee_send_pkt - INTERNAL
   sends a complete packet of data */
void xbee_send_pkt(t_data *pkt) {
  ISREADY;

  /* write and flush the data */
  fwrite(pkt->data,pkt->length,1,xbee.tty);
  fflush(xbee.tty);

#ifdef DEBUG
  {
    int i;
    /* prints packet in hex byte-by-byte */
    printf("XBee: TX Packet - ");
    for (i=0;i<pkt->length;i++) {
      printf("0x%02X ",pkt->data[i]);
    }
    printf("\n");
  }
#endif

  /* free the packet */
  Xfree(pkt);
}

/* #################################################################
   xbee_make_pkt - INTERNAL
   adds delimiter field
   calculates length and checksum
   escapes bytes */
t_data *xbee_make_pkt(unsigned char *data, int length) {
  t_data *pkt;
  unsigned int l, i, o, t, m;
  char d = 0;

  ISREADY;

  /* check the data given isnt too long
     100 bytes maximum payload + 12 bytes header information */
  if (length > 100 + 12) return NULL;

  /* calculate the length of the whole packet
     start, length (MSB), length (LSB), DATA, checksum */
  l = 3 + length + 1;

  /* prepare memory */
  pkt = Xmalloc(sizeof(t_data));

  /* put start byte on */
  pkt->data[0] = 0x7E;

  /* copy data into packet */
  for (t = 0, i = 0, o = 1, m = 1; i <= length; o++, m++) {
    /* if: its time for the checksum */
    if (i == length) d = M8((0xFF - M8(t)));
    /* if: its time for the high length byte */
    else if (m == 1) d = M8(length >> 8);
    /* if: its time for the low length byte */
    else if (m == 2) d = M8(length);
    /* if: its time for the normal data */
    else if (m > 2) d = data[i];

    /* check for any escapes needed */
    if ((d == 0x11) || /* XON */
    	(d == 0x13) || /* XOFF */
    	(d == 0x7D) || /* Escape */
    	(d == 0x7E)) { /* Frame Delimiter */
      l++;
      pkt->data[o++] = 0x7D;
      d ^= 0x20;
    }

    /* move data in */
    pkt->data[o] = d;
    if (m > 2) {
      i++;
      t += d;
    }
  }

  /* remember the length */
  pkt->length = l;

  return pkt;
}
