/**********************************************************************
 * file:  sr_router.c
 *
 * Descripción:
 *
 * Este archivo contiene todas las funciones que interactúan directamente
 * con la tabla de enrutamiento, así como el método de entrada principal
 * para el enrutamiento.
 *
 **********************************************************************/

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "sr_if.h"
#include "sr_rt.h"
#include "sr_router.h"
#include "sr_protocol.h"
#include "sr_arpcache.h"
#include "sr_utils.h"

/*---------------------------------------------------------------------
 * Method: sr_init(void)
 * Scope:  Global
 *
 * Inicializa el subsistema de enrutamiento
 *
 *---------------------------------------------------------------------*/

void sr_init(struct sr_instance *sr)
{
  assert(sr);

  /* Inicializa la caché y el hilo de limpieza de la caché */
  sr_arpcache_init(&(sr->cache));

  /* Inicializa los atributos del hilo */
  pthread_attr_init(&(sr->attr));
  pthread_attr_setdetachstate(&(sr->attr), PTHREAD_CREATE_JOINABLE);
  pthread_attr_setscope(&(sr->attr), PTHREAD_SCOPE_SYSTEM);
  pthread_attr_setscope(&(sr->attr), PTHREAD_SCOPE_SYSTEM);
  pthread_t thread;

  /* Hilo para gestionar el timeout del caché ARP */
  pthread_create(&thread, &(sr->attr), sr_arpcache_timeout, sr);

} /* -- sr_init -- */

struct sr_rt *sr_longest_prefix_match(struct sr_instance *sr, uint32_t dest_ip)
{
  if (sr == NULL || sr->routing_table == NULL)
  {
    printf("SR O SR->RT ES NULL \n");
    return NULL;
  }
  struct sr_rt *longestMatch = NULL;
  uint32_t bestMask = 0; /* Inicializa la mejor máscara a 0 para buscar la coincidencia más larga */
  struct sr_rt *tabla = sr->routing_table;

  while (tabla != NULL)
  {
    /* Aplicar la máscara de red al destino IP y comparar con el prefijo de la entrada */
    uint32_t maskedDest = tabla->mask.s_addr & dest_ip;
    if (maskedDest == (tabla->dest.s_addr & tabla->mask.s_addr))
    {
      /* Si hay coincidencia, verifica si es la coincidencia más larga */
      if (ntohl(tabla->mask.s_addr) > ntohl(bestMask))
      {
        longestMatch = tabla;
        bestMask = tabla->mask.s_addr;
      }
    }

    /* Ir a la siguiente entrada en la tabla de enrutamiento */
    tabla = tabla->next;
  }

  return longestMatch;
}

/* Envía un paquete ICMP de error */
void sr_send_icmp_error_packet(uint8_t type,
                               uint8_t code,
                               struct sr_instance *sr,
                               uint32_t ipDst,
                               uint8_t *ipPacket)
{
  struct sr_rt *rtEntry = sr_longest_prefix_match(sr, ipDst);
  if (rtEntry == NULL)
  {
    printf("No hay entrada en routing table.\n");
    return;
  }

  uint32_t next_hop_ip = (rtEntry->gw.s_addr == 0) ? ipDst : rtEntry->gw.s_addr;

  struct sr_if *out_iface = sr_get_interface(sr, rtEntry->interface);

  if (out_iface == NULL)
  {
    printf("Interfaz de salida no encontrada para %s\n", rtEntry->interface);
    return;
  }
  char *iface_name = out_iface->name;

  /* Determinar el tamaño del paquete ICMP en función del tipo */
  unsigned int icmpLen = sizeof(sr_icmp_t3_hdr_t);
  unsigned int len = sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t) + icmpLen;
  uint8_t *echoReply = malloc(len);
  memset(echoReply, 0, len);

  /*Encabezado Ethernet*/
  sr_ethernet_hdr_t *ethHdr = (sr_ethernet_hdr_t *)echoReply;
  ethHdr->ether_type = htons(ethertype_ip);

  sr_ip_hdr_t *ipHdrAux = (sr_ip_hdr_t *)ipPacket;

  /* Construir encabezado IP */
  sr_ip_hdr_t *ipHdr = (sr_ip_hdr_t *)(echoReply + sizeof(sr_ethernet_hdr_t));
  ipHdr->ip_v = 4;
  ipHdr->ip_hl = 5;
  ipHdr->ip_tos = ipHdrAux->ip_tos;
  ipHdr->ip_len = htons(len - sizeof(sr_ethernet_hdr_t));
  ipHdr->ip_id = ipHdrAux->ip_id;
  ipHdr->ip_off = 0;
  ipHdr->ip_ttl = 64;
  ipHdr->ip_p = ip_protocol_icmp;
  ipHdr->ip_src = out_iface->ip;
  ipHdr->ip_dst = ipDst;
  ipHdr->ip_sum = 0;
  ipHdr->ip_sum = ip_cksum(ipHdr, sizeof(sr_ip_hdr_t));

  /* ICMP Type 3 (Destination Unreachable) */
  sr_icmp_t3_hdr_t *icmpHdr = (sr_icmp_t3_hdr_t *)(echoReply + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t));
  icmpHdr->icmp_type = type;
  icmpHdr->icmp_code = code;
  icmpHdr->icmp_sum = 0;
  icmpHdr->unused = 0;
  icmpHdr->next_mtu = 0;
  memcpy(icmpHdr->data, ipPacket, ICMP_DATA_SIZE);
  icmpHdr->icmp_sum = icmp3_cksum(icmpHdr, sizeof(sr_icmp_t3_hdr_t));

  struct sr_arpentry *arpEntry = sr_arpcache_lookup(&(sr->cache), next_hop_ip);

  if (arpEntry)
  {
    /* Si la entrada existe, usar la dirección MAC de arpEntry */
    memcpy(ethHdr->ether_shost, out_iface->addr, ETHER_ADDR_LEN);
    memcpy(ethHdr->ether_dhost, arpEntry->mac, ETHER_ADDR_LEN);
    is_packet_valid(echoReply, len);
    printf("Paquete a Enviar: \n");
    print_hdrs(echoReply, len);
    sr_send_packet(sr, echoReply, len, out_iface->name);
    /* Liberar la entrada ARP obtenida */
    free(arpEntry);
  }
  else
  {
    /* Si no hay entrada ARP, encolar la solicitud ARP*/
    struct sr_arpreq *req = sr_arpcache_queuereq(&(sr->cache), next_hop_ip, echoReply, len, out_iface->name);
    handle_arpreq(sr, req); /* Maneja la solicitud ARP, enviará el ARP request si es necesario */
  }

  /* Liberar memoria del paquete solo si no se puso en cola */
  if (arpEntry)
  {
    free(echoReply);
  }

} /* -- sr_send_icmp_error_packet -- */

void sr_handle_ip_packet(struct sr_instance *sr,
                         uint8_t *packet /* lent */,
                         unsigned int len,
                         uint8_t *srcAddr,
                         uint8_t *destAddr,
                         char *interface /* lent */,
                         sr_ethernet_hdr_t *eHdr)
{
  /*Obtener el cabezal IP y direcciones*/
  sr_ip_hdr_t *ipHdr = (sr_ip_hdr_t *)(packet + sizeof(sr_ethernet_hdr_t));

  /* Obtengo las direcciones IP */
  uint32_t senderIP = ipHdr->ip_src;
  uint32_t targetIP = ipHdr->ip_dst;

  /* Verifico si el paquete IP es para una de mis interfaces */
  struct sr_if *myInterface = sr_get_interface_given_ip(sr, targetIP);
  if (myInterface != 0)
  {
    /* Es un paquete para el router: manejar ICMP echo request */
    if (ipHdr->ip_p == ip_protocol_icmp)
    {
      sr_icmp_hdr_t *icmpHdr = (sr_icmp_hdr_t *)(packet + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t));
      if (icmpHdr->icmp_type == 8)
      { /* Echo request */
        printf("Es Echo request. \n");
        /*Ajustamos el ICMP*/
        icmpHdr->icmp_type = 0;
        icmpHdr->icmp_code = 0;
        icmpHdr->icmp_sum = 0;
        int icmpLength = len - sizeof(sr_ethernet_hdr_t) - sizeof(sr_ip_hdr_t);
        icmpHdr->icmp_sum = icmp_cksum(icmpHdr, icmpLength);

        /*Ajustamos el IP*/
        ipHdr->ip_src = targetIP;
        ipHdr->ip_dst = senderIP;
        ipHdr->ip_ttl = 64;
        ipHdr->ip_sum = 0;
        ipHdr->ip_sum = ip_cksum(ipHdr, sizeof(sr_ip_hdr_t));

        /*Ajustamos el Ethernet*/
        uint8_t auxEtherDest[ETHER_ADDR_LEN];
        memcpy(auxEtherDest, eHdr->ether_dhost, ETHER_ADDR_LEN);
        memcpy(eHdr->ether_dhost, eHdr->ether_shost, ETHER_ADDR_LEN);
        memcpy(eHdr->ether_shost, auxEtherDest, ETHER_ADDR_LEN);

        printf("Mando echo reply \n");
        print_hdrs(packet, len);
        sr_send_packet(sr, packet, len, interface);
      }
    }
    else
    {
      /* Paquete UDP o TCP, responder con ICMP port unreachable */
      sr_send_icmp_error_packet(3, 3, sr, ipHdr->ip_src, packet + sizeof(sr_ethernet_hdr_t));
    }
    return;
  }

  /* Si no es para este router, disminuir TTL y reenviar */
  ipHdr->ip_ttl--;

  struct sr_rt *rtEntry = sr_longest_prefix_match(sr, ipHdr->ip_dst);
  printf("Longest prefix match: \n");
  if (rtEntry)
  {
    sr_print_routing_entry(rtEntry);
  }
  else
  {
    printf("Entrada no encontrada \n");
    sr_send_icmp_error_packet(3, 0, sr, ipHdr->ip_src, packet + sizeof(sr_ethernet_hdr_t));
    return;
  }

  if (ipHdr->ip_ttl < 1)
  {
    /* Enviar ICMP time exceeded */
    sr_send_icmp_error_packet(11, 0, sr, ipHdr->ip_src, packet + sizeof(sr_ethernet_hdr_t));
    return;
  }

  uint32_t next_hop_ip;
  if (rtEntry->gw.s_addr == 0)
  {
    next_hop_ip = targetIP;
    printf("Dirección IP de destino en el paquete: ");
    print_addr_ip_int(next_hop_ip);
  }
  else
  {
    next_hop_ip = rtEntry->gw.s_addr;
    printf("Gateway en la entrada de la tabla de enrutamiento: ");
    print_addr_ip_int(rtEntry->gw.s_addr);
  }

  sr_icmp_hdr_t *icmpHdr = (sr_icmp_hdr_t *)(packet + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t));
  /* Actualizar suma de comprobación del paquete */
  ipHdr->ip_sum = 0;
  ipHdr->ip_sum = ip_cksum(ipHdr, sizeof(struct sr_ip_hdr));

  struct sr_if *out_iface = sr_get_interface(sr, rtEntry->interface);
  if (out_iface == NULL)
  {
    printf("Error: No se encontró la interfaz %s\n", rtEntry->interface);
    return;
  }

  char *iface_name = out_iface->name;
  printf("Iface name: %s\n", iface_name);

  struct sr_arpentry *arpEntry = sr_arpcache_lookup(&(sr->cache), next_hop_ip);
  if (arpEntry)
  {
    /* Si la entrada existe, usar la dirección MAC de arpEntry */
    sr_ethernet_hdr_t *ethHdr = (sr_ethernet_hdr_t *)packet;
    memcpy(ethHdr->ether_dhost, arpEntry->mac, ETHER_ADDR_LEN);

    struct sr_if *out_interface = sr_get_interface(sr, out_iface);
    memcpy(ethHdr->ether_shost, out_interface->addr, ETHER_ADDR_LEN); /* Origen: MAC de la interfaz de salida */

    /* Enviar el paquete a través de la interfaz de salida */
    sr_send_packet(sr, packet, len, iface_name);

    /* Liberar la entrada ARP obtenida */
    free(arpEntry);
  }
  else
  {
    /* Si no hay entrada ARP, encolar la solicitud ARP
    struct sr_arpreq *req = sr_arpcache_queuereq(&(sr->cache), next_hop.s_addr, packet, len, out_iface); */
    printf("NO ESTA EN CACHE \n");
    printf("Direccion IP proximo salto: ");
    print_addr_ip_int(next_hop_ip);
    printf("Interfaz: ");
    printf("%s\n", iface_name);
    struct sr_arpreq *req = sr_arpcache_queuereq(&(sr->cache), next_hop_ip, packet, len, iface_name);
    handle_arpreq(sr, req); /* Maneja la solicitud ARP, enviará el ARP request si es necesario */
  }
}

/*
 * COLOQUE ASÍ SU CÓDIGO
 * SUGERENCIAS:
 * - Obtener el cabezal IP y direcciones
 * - Verificar si el paquete es para una de mis interfaces o si hay una coincidencia en mi tabla de enrutamiento
 * - Si no es para una de mis interfaces y no hay coincidencia en la tabla de enrutamiento, enviar ICMP net unreachable
 * - Sino, si es para mí, verificar si es un paquete ICMP echo request y responder con un echo reply
 * - Sino, verificar TTL, ARP y reenviar si corresponde (puede necesitar una solicitud ARP y esperar la respuesta)
 * - No olvide imprimir los mensajes de depuración
 */

/*
 * ***** A partir de aquí no debería tener que modificar nada ****
 */

/* Envía todos los paquetes IP pendientes de una solicitud ARP */
void sr_arp_reply_send_pending_packets(struct sr_instance *sr,
                                       struct sr_arpreq *arpReq,
                                       uint8_t *dhost,
                                       uint8_t *shost,
                                       struct sr_if *iface)
{

  struct sr_packet *currPacket = arpReq->packets;
  sr_ethernet_hdr_t *ethHdr;
  uint8_t *copyPacket;

  while (currPacket != NULL)
  {
    ethHdr = (sr_ethernet_hdr_t *)currPacket->buf;
    memcpy(ethHdr->ether_shost, dhost, sizeof(uint8_t) * ETHER_ADDR_LEN);
    memcpy(ethHdr->ether_dhost, shost, sizeof(uint8_t) * ETHER_ADDR_LEN);

    copyPacket = malloc(sizeof(uint8_t) * currPacket->len);
    memcpy(copyPacket, ethHdr, sizeof(uint8_t) * currPacket->len);

    print_hdrs(copyPacket, currPacket->len);
    sr_send_packet(sr, copyPacket, currPacket->len, iface->name);
    currPacket = currPacket->next;
  }
}

/* Gestiona la llegada de un paquete ARP*/
void sr_handle_arp_packet(struct sr_instance *sr,
                          uint8_t *packet /* lent */,
                          unsigned int len,
                          uint8_t *srcAddr,
                          uint8_t *destAddr,
                          char *interface /* lent */,
                          sr_ethernet_hdr_t *eHdr)
{

  /* Imprimo el cabezal ARP */
  printf("*** -> It is an ARP packet. Print ARP header.\n");
  print_hdr_arp(packet + sizeof(sr_ethernet_hdr_t));

  /* Obtengo el cabezal ARP */
  sr_arp_hdr_t *arpHdr = (sr_arp_hdr_t *)(packet + sizeof(sr_ethernet_hdr_t));

  /* Obtengo las direcciones MAC */
  unsigned char senderHardAddr[ETHER_ADDR_LEN], targetHardAddr[ETHER_ADDR_LEN];
  memcpy(senderHardAddr, arpHdr->ar_sha, ETHER_ADDR_LEN);
  memcpy(targetHardAddr, arpHdr->ar_tha, ETHER_ADDR_LEN);

  /* Obtengo las direcciones IP */
  uint32_t senderIP = arpHdr->ar_sip;
  uint32_t targetIP = arpHdr->ar_tip;
  unsigned short op = ntohs(arpHdr->ar_op);

  /* Verifico si el paquete ARP es para una de mis interfaces */
  struct sr_if *myInterface = sr_get_interface_given_ip(sr, targetIP);

  if (op == arp_op_request)
  { /* Si es un request ARP */
    printf("**** -> It is an ARP request.\n");

    /* Si el ARP request es para una de mis interfaces */
    if (myInterface != 0)
    {
      printf("***** -> ARP request is for one of my interfaces.\n");

      /* Agrego el mapeo MAC->IP del sender a mi caché ARP */
      printf("****** -> Add MAC->IP mapping of sender to my ARP cache.\n");
      sr_arpcache_insert(&(sr->cache), senderHardAddr, senderIP);

      /* Construyo un ARP reply y lo envío de vuelta */
      printf("****** -> Construct an ARP reply and send it back.\n");
      memcpy(eHdr->ether_shost, (uint8_t *)myInterface->addr, sizeof(uint8_t) * ETHER_ADDR_LEN);
      memcpy(eHdr->ether_dhost, (uint8_t *)senderHardAddr, sizeof(uint8_t) * ETHER_ADDR_LEN);
      memcpy(arpHdr->ar_sha, myInterface->addr, ETHER_ADDR_LEN);
      memcpy(arpHdr->ar_tha, senderHardAddr, ETHER_ADDR_LEN);
      arpHdr->ar_sip = targetIP;
      arpHdr->ar_tip = senderIP;
      arpHdr->ar_op = htons(arp_op_reply);

      /* Imprimo el cabezal del ARP reply creado */
      print_hdrs(packet, len);

      sr_send_packet(sr, packet, len, myInterface->name);
    }

    printf("******* -> ARP request processing complete.\n");
  }
  else if (op == arp_op_reply)
  { /* Si es un reply ARP */

    printf("**** -> It is an ARP reply.\n");

    /* Agrego el mapeo MAC->IP del sender a mi caché ARP */
    printf("***** -> Add MAC->IP mapping of sender to my ARP cache.\n");
    struct sr_arpreq *arpReq = sr_arpcache_insert(&(sr->cache), senderHardAddr, senderIP);

    if (arpReq != NULL)
    { /* Si hay paquetes pendientes */

      printf("****** -> Send outstanding packets.\n");
      sr_arp_reply_send_pending_packets(sr, arpReq, (uint8_t *)myInterface->addr, (uint8_t *)senderHardAddr, myInterface);
      sr_arpreq_destroy(&(sr->cache), arpReq);
    }
    printf("******* -> ARP reply processing complete.\n");
  }
}

/*---------------------------------------------------------------------
 * Method: sr_handlepacket(uint8_t* p,char* interface)
 * Scope:  Global
 *
 * This method is called each time the router receives a packet on the
 * interface.  The packet buffer, the packet length and the receiving
 * interface are passed in as parameters. The packet is complete with
 * ethernet headers.
 *
 * Note: Both the packet buffer and the character's memory are handled
 * by sr_vns_comm.c that means do NOT delete either.  Make a copy of the
 * packet instead if you intend to keep it around beyond the scope of
 * the method call.
 *
 *---------------------------------------------------------------------*/

void sr_handlepacket(struct sr_instance *sr,
                     uint8_t *packet /* lent */,
                     unsigned int len,
                     char *interface /* lent */)
{
  assert(sr);
  assert(packet);
  assert(interface);

  printf("*** -> Received packet of length %d \n", len);

  /* Obtengo direcciones MAC origen y destino */
  sr_ethernet_hdr_t *eHdr = (sr_ethernet_hdr_t *)packet;
  uint8_t *destAddr = malloc(sizeof(uint8_t) * ETHER_ADDR_LEN);
  uint8_t *srcAddr = malloc(sizeof(uint8_t) * ETHER_ADDR_LEN);
  memcpy(destAddr, eHdr->ether_dhost, sizeof(uint8_t) * ETHER_ADDR_LEN);
  memcpy(srcAddr, eHdr->ether_shost, sizeof(uint8_t) * ETHER_ADDR_LEN);
  uint16_t pktType = ntohs(eHdr->ether_type);

  if (is_packet_valid(packet, len))
  {
    if (pktType == ethertype_arp)
    {
      sr_handle_arp_packet(sr, packet, len, srcAddr, destAddr, interface, eHdr);
    }
    else if (pktType == ethertype_ip)
    {
      sr_handle_ip_packet(sr, packet, len, srcAddr, destAddr, interface, eHdr);
    }
  }

} /* end sr_ForwardPacket */
