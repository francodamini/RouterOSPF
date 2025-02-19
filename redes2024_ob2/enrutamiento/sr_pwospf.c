/*-----------------------------------------------------------------------------
 * file: sr_pwospf.c
 *
 * Descripción:
 * Este archivo contiene las funciones necesarias para el manejo de los paquetes
 * OSPF.
 *
 *---------------------------------------------------------------------------*/

#include "sr_pwospf.h"
#include "sr_router.h"

#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <malloc.h>

#include <string.h>
#include <time.h>
#include <stdlib.h>

#include "sr_utils.h"
#include "sr_protocol.h"
#include "pwospf_protocol.h"
#include "sr_rt.h"
#include "pwospf_neighbors.h"
#include "pwospf_topology.h"
#include "dijkstra.h"

/*pthread_t hello_thread;*/
pthread_t g_hello_packet_thread;
pthread_t g_all_lsu_thread;
pthread_t g_lsu_thread;
pthread_t g_neighbors_thread;
pthread_t g_topology_entries_thread;
pthread_t g_rx_lsu_thread;
pthread_t g_dijkstra_thread;

pthread_mutex_t g_dijkstra_mutex = PTHREAD_MUTEX_INITIALIZER;

struct in_addr g_router_id;
uint8_t g_ospf_multicast_mac[ETHER_ADDR_LEN];
struct ospfv2_neighbor *g_neighbors;
struct pwospf_topology_entry *g_topology;
uint16_t g_sequence_num;

/* -- Declaración de hilo principal de la función del subsistema pwospf --- */
static void *pwospf_run_thread(void *arg);

/*---------------------------------------------------------------------
 * Method: pwospf_init(..)
 *
 * Configura las estructuras de datos internas para el subsistema pwospf
 * y crea un nuevo hilo para el subsistema pwospf.
 *
 * Se puede asumir que las interfaces han sido creadas e inicializadas
 * en este punto.
 *---------------------------------------------------------------------*/

int pwospf_init(struct sr_instance *sr)
{
    assert(sr);
    sr->ospf_subsys = (struct pwospf_subsys *)malloc(sizeof(struct
                                                            pwospf_subsys));

    assert(sr->ospf_subsys);
    pthread_mutex_init(&(sr->ospf_subsys->lock), 0);

    g_router_id.s_addr = 0;

    /* Defino la MAC de multicast a usar para los paquetes HELLO */
    g_ospf_multicast_mac[0] = 0x01;
    g_ospf_multicast_mac[1] = 0x00;
    g_ospf_multicast_mac[2] = 0x5e;
    g_ospf_multicast_mac[3] = 0x00;
    g_ospf_multicast_mac[4] = 0x00;
    g_ospf_multicast_mac[5] = 0x05;

    g_neighbors = NULL;

    g_sequence_num = 0;

    struct in_addr zero;
    zero.s_addr = 0;
    g_neighbors = create_ospfv2_neighbor(zero);
    g_topology = create_ospfv2_topology_entry(zero, zero, zero, zero, zero, 0);

    /* -- start thread subsystem -- */
    if (pthread_create(&sr->ospf_subsys->thread, 0, pwospf_run_thread, sr))
    {
        perror("pthread_create");
        assert(0);
    }

    return 0; /* success */
} /* -- pwospf_init -- */

/*---------------------------------------------------------------------
 * Method: pwospf_lock
 *
 * Lock mutex associated with pwospf_subsys
 *
 *---------------------------------------------------------------------*/

void pwospf_lock(struct pwospf_subsys *subsys)
{
    if (pthread_mutex_lock(&subsys->lock))
    {
        assert(0);
    }
}

/*---------------------------------------------------------------------
 * Method: pwospf_unlock
 *
 * Unlock mutex associated with pwospf subsystem
 *
 *---------------------------------------------------------------------*/

void pwospf_unlock(struct pwospf_subsys *subsys)
{
    if (pthread_mutex_unlock(&subsys->lock))
    {
        assert(0);
    }
}

/*---------------------------------------------------------------------
 * Method: pwospf_run_thread
 *
 * Hilo principal del subsistema pwospf.
 *
 *---------------------------------------------------------------------*/

static void *pwospf_run_thread(void *arg)
{
    sleep(5);

    struct sr_instance *sr = (struct sr_instance *)arg;

    /* Set the ID of the router */
    while (g_router_id.s_addr == 0)
    {
        struct sr_if *int_temp = sr->if_list;
        while (int_temp != NULL)
        {
            if (int_temp->ip > g_router_id.s_addr)
            {
                g_router_id.s_addr = int_temp->ip;
            }

            int_temp = int_temp->next;
        }
    }
    Debug("\n\nPWOSPF: Selecting the highest IP address on a router as the router ID\n");
    Debug("-> PWOSPF: The router ID is [%s]\n", inet_ntoa(g_router_id));

    Debug("\nPWOSPF: Detecting the router interfaces and adding their networks to the routing table\n");
    struct sr_if *int_temp = sr->if_list;
    while (int_temp != NULL)
    {
        struct in_addr ip;
        ip.s_addr = int_temp->ip;
        struct in_addr gw;
        gw.s_addr = 0x00000000;
        struct in_addr mask;
        mask.s_addr = int_temp->mask;
        struct in_addr network;
        network.s_addr = ip.s_addr & mask.s_addr;

        if (check_route(sr, network) == 0)
        {
            Debug("-> PWOSPF: Adding the directly connected network [%s, ", inet_ntoa(network));
            Debug("%s] to the routing table\n", inet_ntoa(mask));
            sr_add_rt_entry(sr, network, gw, mask, int_temp->name, 1);
        }
        int_temp = int_temp->next;
    }

    Debug("\n-> PWOSPF: Printing the forwarding table\n");
    sr_print_routing_table(sr);
    pthread_create(&g_hello_packet_thread, NULL, send_hellos, sr);
    pthread_create(&g_all_lsu_thread, NULL, send_all_lsu, sr);
    pthread_create(&g_neighbors_thread, NULL, check_neighbors_life, sr);
    pthread_create(&g_topology_entries_thread, NULL, check_topology_entries_age, sr);

    return NULL;
} /* -- run_ospf_thread -- */

/***********************************************************************************
 * Métodos para el manejo de los paquetes HELLO y LSU
 * SU CÓDIGO DEBERÍA IR AQUÍ
 * *********************************************************************************/

/*---------------------------------------------------------------------
 * Method: check_neighbors_life
 *
 * Chequea si los vecinos están vivos
 *
 *---------------------------------------------------------------------*/

void *check_neighbors_life(void *arg)
{
    struct sr_instance *sr = (struct sr_instance *)arg;
    /*
    Si hay un cambio, se debe ajustar el neighbor id en la interfaz.
    */
    struct in_addr ip_addr;
    ip_addr.s_addr = 0;
    /*aux es un nodo auxiliar para la eliminación de vecinos*/
    struct ospfv2_neighbor *aux = create_ospfv2_neighbor(ip_addr);
    while (1)
    {
        /*Cada 1 segundo, chequea la lista de vecinos.*/
        usleep(1000000);

        /*lista con todos los vecinos inactivos*/
        struct ospfv2_neighbor *vecino = check_neighbors_alive(g_neighbors);

        /* Si hay un cambio, se debe ajustar el neighbor id en la interfaz. */
        while (vecino != NULL)
        {
            struct sr_if *interfaz = sr->if_list;

            /*Este bucle encuentra la interfaz correcta asociada al vecino.*/
            while (interfaz->neighbor_id != vecino->neighbor_id.s_addr)
            {
                interfaz = interfaz->next;
            }

            /*se actualiza para reflejar que ya no tiene un vecino asociado*/
            interfaz->neighbor_id = 0;
            interfaz->neighbor_ip = 0;

            /* Elimino el vecino */
            aux->next = vecino;
            vecino = vecino->next;
            delete_neighbor(aux);
        }
    };
    return NULL;

} /* -- check_neighbors_life -- */

/*---------------------------------------------------------------------
 * Method: check_topology_entries_age
 *
 * Check if the topology entries are alive
 * and if they are not, remove them from the topology table
 *
 *---------------------------------------------------------------------*/

void *check_topology_entries_age(void *arg)
{
    struct sr_instance *sr = (struct sr_instance *)arg;

    while (1)
    {
        /*Cada 1 segundo, chequea el tiempo de vida de cada entrada de la topologia.*/
        usleep(1000000);
        if (check_topology_age(g_topology) == 1)
        {
            /*Si hay un cambio en la topología, se llama a la función de Dijkstra en un nuevo hilo.*/
            Debug("\n-> PWOSPF: Printing the topology table\n");
            print_topolgy_table(g_topology);
            Debug("\n");

            struct dijkstra_param *dij_param = ((dijkstra_param_t *)(malloc(sizeof(dijkstra_param_t))));
            dij_param->sr = sr;
            dij_param->topology = g_topology;
            dij_param->mutex = g_dijkstra_mutex;
            dij_param->rid = g_router_id;
            pthread_create(&g_dijkstra_thread, NULL, run_dijkstra, dij_param);
        }
    };

    return NULL;
} /* -- check_topology_entries_age -- */

/*---------------------------------------------------------------------
 * Method: send_hellos
 *
 * Para cada interfaz y cada helloint segundos, construye mensaje
 * HELLO y crea un hilo con la función para enviar el mensaje.
 *
 *---------------------------------------------------------------------*/

void *send_hellos(void *arg)
{
    struct sr_instance *sr = (struct sr_instance *)arg;

    /* While true */
    while (1)
    {
        /* Se ejecuta cada 1 segundo */
        usleep(1000000);

        /* Bloqueo para evitar mezclar el envío de HELLOs y LSUs */
        pwospf_lock(sr->ospf_subsys);

        /* Chequeo todas las interfaces para enviar el paquete HELLO */
        /* Cada interfaz matiene un contador en segundos para los HELLO*/
        /* Reiniciar el contador de segundos para HELLO */

        /* Chequeo todas las interfaces para enviar el paquete HELLO */
        struct sr_if *Interfaces = sr->if_list;
        while (Interfaces != NULL)
        {
            if (Interfaces->helloint > 0)
            {
                Interfaces->helloint--;
            }
            if (Interfaces->helloint == 0)
            {
                struct powspf_hello_lsu_param *hello_param = ((powspf_hello_lsu_param_t *)(malloc(sizeof(powspf_hello_lsu_param_t))));
                hello_param->sr = sr;
                hello_param->interface = Interfaces;
                /* Crear un hilo para enviar el paquete HELLO */
                pthread_create(&g_hello_packet_thread, NULL, send_hello_packet, hello_param);
                /*send_hello_packet(hello_param);*/
                /* Reiniciar el contador de segundos para HELLO */
                Interfaces->helloint = OSPF_DEFAULT_HELLOINT;
            }
            Interfaces = Interfaces->next;
        }

        /* Desbloqueo */
        pwospf_unlock(sr->ospf_subsys);
    };

    return NULL;
} /* -- send_hellos -- */

/*---------------------------------------------------------------------
 * Method: send_hello_packet
 *
 * Recibe un mensaje HELLO, agrega cabezales y lo envía por la interfaz
 * correspondiente.
 *
 *---------------------------------------------------------------------*/

void *send_hello_packet(void *arg)
{
    powspf_hello_lsu_param_t *hello_param = ((powspf_hello_lsu_param_t *)(arg));

    Debug("\n\nPWOSPF: Constructing HELLO packet for interface %s: \n", hello_param->interface->name);

    int len = sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t) + sizeof(ospfv2_hdr_t) + sizeof(ospfv2_hello_hdr_t);
    uint8_t *hello_packet = malloc(len);

    sr_ethernet_hdr_t *e_hdr = ((sr_ethernet_hdr_t *)hello_packet);
    /* Seteo la dirección MAC de multicast para la trama a enviar */
    memcpy(e_hdr->ether_dhost, g_ospf_multicast_mac, ETHER_ADDR_LEN);

    /* Seteo la dirección MAC origen con la dirección de mi interfaz de salida */
    memcpy(e_hdr->ether_shost, hello_param->interface->addr, ETHER_ADDR_LEN);

    /* Seteo el ether_type en el cabezal Ethernet */
    e_hdr->ether_type = htons(ethertype_ip);

    /* Inicializo cabezal IP */
    sr_ip_hdr_t *ip_hdr = ((sr_ip_hdr_t *)(hello_packet + sizeof(sr_ethernet_hdr_t)));

    ip_hdr->ip_v = 4;
    ip_hdr->ip_hl = 5;
    ip_hdr->ip_tos = 0;
    ip_hdr->ip_len = htons(sizeof(struct sr_ip_hdr) + sizeof(struct ospfv2_hdr) + sizeof(struct ospfv2_hello_hdr));
    ip_hdr->ip_id = 0;
    ip_hdr->ip_ttl = 64;

    /* Seteo el protocolo en el cabezal IP para ser el de OSPF (89) */
    ip_hdr->ip_p = ip_protocol_ospfv2;

    /* Seteo IP origen con la IP de mi interfaz de salida */
    ip_hdr->ip_src = hello_param->interface->ip;

    /* Seteo IP destino con la IP de Multicast dada: OSPF_AllSPFRouters  */
    ip_hdr->ip_dst = ntohl(OSPF_AllSPFRouters);

    /* Calculo y seteo el chechsum IP*/
    ip_hdr->ip_sum = 0;
    ip_hdr->ip_sum = ip_cksum(ip_hdr, sizeof(sr_ip_hdr_t));

    /* Inicializo cabezal de PWOSPF con version 2 y tipo HELLO */
    ospfv2_hdr_t *ospfv2_hdr = ((ospfv2_hdr_t *)(hello_packet + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t)));
    ospfv2_hdr->version = OSPF_V2;
    ospfv2_hdr->type = OSPF_TYPE_HELLO;
    ospfv2_hdr->len = htons(sizeof(ospfv2_hdr_t) + sizeof(ospfv2_hello_hdr_t));

    /* Seteo el Router ID con mi ID*/
    ospfv2_hdr->rid = g_router_id.s_addr;

    /* Seteo el Area ID en 0 */
    ospfv2_hdr->aid = 0;

    /* Seteo el Authentication Type y Authentication Data en 0*/
    ospfv2_hdr->autype = 0;
    ospfv2_hdr->audata = 0;

    /* Seteo máscara con la máscara de mi interfaz de salida */
    ospfv2_hello_hdr_t *ospfv2_hello_hdr = ((ospfv2_hello_hdr_t *)(hello_packet + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t) + sizeof(ospfv2_hdr_t)));
    ospfv2_hello_hdr->nmask = hello_param->interface->mask;

    /* Seteo Hello Interval con OSPF_DEFAULT_HELLOINT */
    ospfv2_hello_hdr->helloint = OSPF_DEFAULT_HELLOINT;

    /* Seteo Padding en 0*/
    ospfv2_hello_hdr->padding = 0;

    /* Creo el paquete a transmitir */

    /* Calculo y actualizo el checksum del cabezal OSPF */
    ospfv2_hdr->csum = 0;
    ospfv2_hdr->csum = ospfv2_cksum(ospfv2_hdr, sizeof(ospfv2_hdr_t) + sizeof(ospfv2_hello_hdr_t));

    /*print_hdrs(hello_packet, len);*/

    /* Envío el paquete HELLO */
    /*sr_send_packet(hello_param->sr, ((uint8_t *)(hello_packet)), len, hello_param->interface->name);*/
    sr_send_packet(hello_param->sr, hello_packet, len, hello_param->interface->name);
    /* Imprimo información del paquete HELLO enviado */

    Debug("-> PWOSPF: Sending HELLO Packet of length = %d, out of the interface: %s\n", len, hello_param->interface->name);
    Debug("      [Router ID = %s]\n", inet_ntoa(g_router_id));
    Debug("      [Router IP = %s]\n", inet_ntoa(*(struct in_addr *)&ip_hdr->ip_src));
    Debug("      [Network Mask = %s]\n", inet_ntoa(*(struct in_addr *)&ospfv2_hello_hdr->nmask));

    return NULL;
} /* -- send_hello_packet -- */

/*---------------------------------------------------------------------
 * Method: send_all_lsu
 *
 * Construye y envía LSUs cada 30 segundos
 *
 *---------------------------------------------------------------------*/

void *send_all_lsu(void *arg)
{
    struct sr_instance *sr = (struct sr_instance *)arg;

    /* while true*/
    while (1)
    {
        /* Se ejecuta cada OSPF_DEFAULT_LSUINT segundos */
        usleep(OSPF_DEFAULT_LSUINT * 1000000);

        /* Bloqueo para evitar mezclar el envío de HELLOs y LSUs */
        pwospf_lock(sr->ospf_subsys);

        /* Recorro todas las interfaces para enviar el paquete LSU */
        /* Si la interfaz tiene un vecino, envío un LSU */
        /* Recorro todas las interfaces para enviar el paquete LSU */
        /*struct sr_if *Interfaces = (struct sr_if *)sr->if_list;*/
        struct sr_if *Interfaces = sr->if_list;
        while (Interfaces != NULL)
        {
            /* Si la interfaz tiene un vecino, envío un LSU */
            if (Interfaces->neighbor_id)
            {
                powspf_hello_lsu_param_t *lsu_param = malloc(sizeof(powspf_hello_lsu_param_t));
                lsu_param->interface = Interfaces;
                lsu_param->sr = sr;
                send_lsu(lsu_param);
            }
            Interfaces = Interfaces->next;
        }

        g_sequence_num++;

        /* Desbloqueo */
        pwospf_unlock(sr->ospf_subsys);
    };

    return NULL;
} /* -- send_all_lsu -- */

/*---------------------------------------------------------------------
 * Method: send_lsu
 *
 * Construye y envía paquetes LSU a través de una interfaz específica
 *
 *---------------------------------------------------------------------*/

void *send_lsu(void *arg)
{
    powspf_hello_lsu_param_t *lsu_param = ((powspf_hello_lsu_param_t *)(arg));

     if (lsu_param->interface->neighbor_ip == 0)
    {
        return NULL;
    }
    else {
        printf("No es cero \n");
        sr_print_if(lsu_param->interface);
    }

    /* Construyo el LSU */
    Debug("\n\nPWOSPF: Constructing LSU packet\n");

    /*PAquete LSU*/
    int cantidad_rutas = count_routes(lsu_param->sr);

    int len = sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t) + sizeof(ospfv2_hdr_t) + sizeof(ospfv2_lsu_hdr_t) + (cantidad_rutas * sizeof(ospfv2_lsa_t));
    uint8_t *lsu_packet = malloc(len);
    memset(lsu_packet, 0, len);

    /* Inicializo cabezal Ethernet */
    /* Dirección MAC destino la dejo para el final ya que hay que hacer ARP */
    sr_ethernet_hdr_t *tx_e_hdr = ((sr_ethernet_hdr_t *)(lsu_packet));
    sr_ip_hdr_t *tx_ip_hdr = ((sr_ip_hdr_t *)(sizeof(sr_ethernet_hdr_t) + lsu_packet));
    ospfv2_hdr_t *tx_ospf_hdr = ((ospfv2_hdr_t *)(sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t) + lsu_packet));
    ospfv2_lsu_hdr_t *tx_ospf_lsu_hdr = ((ospfv2_lsu_hdr_t *)(sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t) + sizeof(ospfv2_hdr_t) + lsu_packet));
    ospfv2_lsa_t *tx_ospf_lsa = ((ospfv2_lsa_t *)(sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t) + sizeof(ospfv2_hdr_t) + sizeof(ospfv2_lsu_hdr_t) + lsu_packet));

    /*Ethernet*/
    memcpy(tx_e_hdr->ether_shost, lsu_param->interface->addr, ETHER_ADDR_LEN);
    tx_e_hdr->ether_type = htons(ethertype_ip);

    /*IP*/
    tx_ip_hdr->ip_v = 4;
    tx_ip_hdr->ip_hl = 5;
    tx_ip_hdr->ip_tos = 0;
    tx_ip_hdr->ip_len = htons(len - sizeof(sr_ethernet_hdr_t));
    tx_ip_hdr->ip_id = 0;
    tx_ip_hdr->ip_off = htons(IP_DF);
    tx_ip_hdr->ip_ttl = 64;
    tx_ip_hdr->ip_p = ip_protocol_ospfv2;
    tx_ip_hdr->ip_sum = 0;
    tx_ip_hdr->ip_src = lsu_param->interface->ip;
    tx_ip_hdr->ip_dst = lsu_param->interface->neighbor_ip;
    tx_ip_hdr->ip_sum = ip_cksum(tx_ip_hdr, sizeof(sr_ip_hdr_t));

    /*OSPF*/
    tx_ospf_hdr->version = OSPF_V2;
    tx_ospf_hdr->type = OSPF_TYPE_LSU;
    tx_ospf_hdr->len = htons(len - sizeof(sr_ethernet_hdr_t) - sizeof(sr_ip_hdr_t));
    tx_ospf_hdr->rid = g_router_id.s_addr;
    tx_ospf_hdr->aid = 0;
    tx_ospf_hdr->csum = 0;
    tx_ospf_hdr->autype = 0;
    tx_ospf_hdr->audata = 0;

    /*LSU*/
    tx_ospf_lsu_hdr->seq = g_sequence_num;
    tx_ospf_lsu_hdr->ttl = 64;
    tx_ospf_lsu_hdr->unused = 0;
    tx_ospf_lsu_hdr->num_adv = cantidad_rutas;



    /***** Creating the transmitted packet *****/

    /* Creo cada LSA iterando en las entradas de la tabla */
    if (lsu_param && lsu_param->sr && lsu_param->sr->routing_table)
    {
        struct sr_rt *entry = lsu_param->sr->routing_table;

        while (entry != NULL)
        {
            printf("ENTRADA DE LA TABLA \n");
            sr_print_routing_entry(entry);
            /* Solo envío entradas directamente conectadas y agreagadas a mano*/
            if (entry->admin_dst <= 1)
            {
                /* Creo LSA con subnet, mask y routerID (id del vecino de la interfaz)*/
                /* Subnet */
                tx_ospf_lsa->subnet = entry->mask.s_addr & entry->dest.s_addr;

                /* Mask */
                tx_ospf_lsa->mask = entry->mask.s_addr;

                /* Router ID */
                tx_ospf_lsa->rid = sr_get_interface(lsu_param->sr, entry->interface)->neighbor_id;
                tx_ospf_lsa = tx_ospf_lsa + 1;

                /*memcpy(tx_packet + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t) + sizeof(ospfv2_hdr_t) + sizeof(ospfv2_lsu_hdr_t) + (sizeof(ospfv2_lsa_t) * i),
                       tx_ospf_lsa, sizeof(ospfv2_lsa_t));

                i++;*/
            }
            entry = entry->next;
        }
    }
    else
    {
        printf("ALGO ES NULL \n");
    }

    tx_ospf_hdr->csum = ospfv2_cksum(tx_ospf_hdr, len - sizeof(sr_ethernet_hdr_t) - sizeof(sr_ip_hdr_t));

    /* Me falta la MAC para poder enviar el paquete, la busco en la cache ARP*/
    struct sr_arpentry *arpEntry = sr_arpcache_lookup(&(lsu_param->sr->cache), lsu_param->interface->neighbor_ip);
    

    if (arpEntry != NULL)
    {
        printf("HAY ARP ENTRY \n");
        printf("PAQUETE LSU \n");
        /* Si la entrada existe, usar la dirección MAC de arpEntry */
        memcpy(tx_e_hdr->ether_dhost, arpEntry->mac, ETHER_ADDR_LEN);
        sr_send_packet(lsu_param->sr, lsu_packet, len, lsu_param->interface->name);
        /* Liberar la entrada ARP obtenida */
        free(arpEntry);
    }
    else
    {
        printf("NO HAY ARP ENTRY \n");
        /* Si no hay entrada ARP, encolar la solicitud ARP*/
        print_hdrs(lsu_packet , len);
        struct sr_arpreq *req = sr_arpcache_queuereq(&(lsu_param->sr->cache), lsu_param->interface->neighbor_ip, lsu_packet, len, lsu_param->interface->name);
        handle_arpreq(lsu_param->sr, req); /* Maneja la solicitud ARP, enviará el ARP request si es necesario */
    }

    return NULL;
} /* -- send_lsu -- */

/*---------------------------------------------------------------------
 * Method: sr_handle_pwospf_hello_packet
 *
 * Gestiona los paquetes HELLO recibidos
 *
 *---------------------------------------------------------------------*/

void sr_handle_pwospf_hello_packet(struct sr_instance *sr, uint8_t *packet, unsigned int length, struct sr_if *rx_if)
{
    /*Obtengo headers del paquete*/
    sr_ip_hdr_t *ip_hdr = ((sr_ip_hdr_t *)(packet + sizeof(sr_ethernet_hdr_t)));
    ospfv2_hdr_t *ospf_hdr = (ospfv2_hdr_t *)(packet + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t));
    ospfv2_hello_hdr_t *hello_hdr = (ospfv2_hello_hdr_t *)(packet + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t) + sizeof(ospfv2_hdr_t));

    struct in_addr neighbor_id;
    neighbor_id.s_addr = ospf_hdr->rid;
    struct in_addr neighbor_ip;
    /*neighbor_ip.s_addr = ((sr_ip_hdr_t *)(packet + sizeof(sr_ethernet_hdr_t)))->ip_src;*/
    neighbor_ip.s_addr = ip_hdr->ip_src;
    struct in_addr net_mask;
    net_mask.s_addr = hello_hdr->nmask;
    /*uint16_t hello_interval = ntohs(hello_hdr->helloint);*/

    /* Imprimir información del paquete recibido */
    Debug("-> PWOSPF: Detecting PWOSPF HELLO Packet from:\n");
    Debug("      [Neighbor ID = %s]\n", inet_ntoa(*(struct in_addr *)&neighbor_id));
    Debug("      [Neighbor IP = %s]\n", inet_ntoa(*(struct in_addr *)&neighbor_ip));
    Debug("      [Network Mask = %s]\n", inet_ntoa(*(struct in_addr *)&net_mask));

    if (!is_packet_valid(packet, length))
    {
        Debug("-> PWOSPF: HELLO Packet dropped, invalid\n");
        return;
    }

    if (hello_hdr->nmask != rx_if->mask) /*net_mask.s_addr != rx_if->mask*/
    {
        Debug("-> PWOSPF: HELLO Packet dropped, invalid hello network mask\n");
        return;
    }

    /* Chequeo del intervalo de HELLO */
    if (hello_hdr->helloint != OSPF_DEFAULT_HELLOINT)
    {
        Debug("-> PWOSPF: HELLO Packet dropped, invalid hello interval\n");
        return;
    }

    struct ospfv2_neighbor *vecino = g_neighbors;

    while (vecino != NULL && ospf_hdr->rid != vecino->neighbor_id.s_addr)
    {
        vecino = vecino->next;
    }

    /*int new_neighbor = 0;
    if (rx_if->neighbor_id != ospf_hdr->rid)
    {
        rx_if->neighbor_id = ospf_hdr->rid;
        new_neighbor = 1;
    }*/
    rx_if->neighbor_id = ospf_hdr->rid;
    rx_if->neighbor_ip = ip_hdr->ip_src;
    rx_if->mask = hello_hdr->nmask;

    struct in_addr id_neighbor;
    id_neighbor.s_addr = ospf_hdr->rid;
    if (vecino)
    {
        refresh_neighbors_alive(g_neighbors, id_neighbor);
    }
    else
    {
        /*if (new_neighbor == 1)*/
        add_neighbor(g_neighbors, create_ospfv2_neighbor(id_neighbor));
        struct sr_if *Iter = sr->if_list;
        while (Iter != NULL)
        {
            if (Iter->neighbor_id)
            {
                powspf_hello_lsu_param_t *lsu_param = malloc(sizeof(powspf_hello_lsu_param_t));
                lsu_param->sr = sr;
                lsu_param->interface = Iter;
                pthread_create(&g_lsu_thread, NULL, send_lsu, lsu_param);

            }
            Iter = Iter->next;
        }
        g_sequence_num++;
    }
    /* Obtengo información del paquete recibido */
    /* Imprimo info del paquete recibido*/
    /*
    Debug("-> PWOSPF: Detecting PWOSPF HELLO Packet from:\n");
    Debug("      [Neighbor ID = %s]\n", inet_ntoa(neighbor_id));
    Debug("      [Neighbor IP = %s]\n", inet_ntoa(neighbor_ip));
    Debug("      [Network Mask = %s]\n", inet_ntoa(net_mask));
    */

    /* Chequeo checksum */
    /*Debug("-> PWOSPF: HELLO Packet dropped, invalid checksum\n");*/

    /* Chequeo de la máscara de red */
    /*Debug("-> PWOSPF: HELLO Packet dropped, invalid hello network mask\n");*/

    /* Chequeo del intervalo de HELLO */
    /*Debug("-> PWOSPF: HELLO Packet dropped, invalid hello interval\n");*/

    /* Seteo el vecino en la interfaz por donde llegó y actualizo la lista de vecinos */

    /* Si es un nuevo vecino, debo enviar LSUs por todas mis interfaces*/
    /* Recorro todas las interfaces para enviar el paquete LSU */
    /* Si la interfaz tiene un vecino, envío un LSU */

} /* -- sr_handle_pwospf_hello_packet -- */

/*---------------------------------------------------------------------
 * Method: sr_handle_pwospf_lsu_packet
 *
 * Gestiona los paquetes LSU recibidos y actualiza la tabla de topología
 * y ejecuta el algoritmo de Dijkstra
 *
 *---------------------------------------------------------------------*/

void *sr_handle_pwospf_lsu_packet(void *arg)
{
    powspf_rx_lsu_param_t *rx_lsu_param = ((powspf_rx_lsu_param_t *)(arg));

    /* Obtengo el vecino que me envió el LSU*/
    sr_ip_hdr_t *ip_hdr = ((sr_ip_hdr_t *)(rx_lsu_param->packet + sizeof(sr_ethernet_hdr_t)));
    struct ospfv2_hdr *rx_ospfv2_hdr = ((struct ospfv2_hdr *)(rx_lsu_param->packet + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t)));
    struct ospfv2_lsu_hdr *rx_ospfv2_lsu_hdr = ((struct ospfv2_lsu_hdr *)(rx_lsu_param->packet + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t) +
                                                                          sizeof(ospfv2_hdr_t)));
    struct ospfv2_lsa *rx_ospfv2_lsa;

    struct in_addr neighbor_id;
    neighbor_id.s_addr = rx_lsu_param->rx_if->neighbor_id;  

    struct in_addr neighbor_ip;
    neighbor_ip.s_addr = rx_lsu_param->rx_if->neighbor_ip;

    /* Imprimo info del paquete recibido*/
    Debug("-> PWOSPF: Detecting LSU Packet from [Neighbor ID = %s, IP = %s]\n", inet_ntoa(neighbor_id), inet_ntoa(neighbor_ip));

    /* Chequeo checksum */
    if (!is_packet_valid(rx_lsu_param->packet, rx_lsu_param->length))
    {
        Debug("-> PWOSPF: LSU Packet dropped, invalid checksum\n");
        return NULL;
    }

    /* Obtengo el Router ID del router originario del LSU y chequeo si no es mío*/
    if (rx_ospfv2_hdr->rid == g_router_id.s_addr)
    {
        Debug("-> PWOSPF: LSU Packet dropped, originated by this router\n");
        return NULL;
    }

    /* Obtengo el número de secuencia y uso check_sequence_number para ver si ya lo recibí desde ese vecino*/
    if (!check_sequence_number(g_topology, neighbor_id, rx_ospfv2_lsu_hdr->seq))
    {
        Debug("-> PWOSPF: LSU Packet dropped, repeated sequence number\n");
        return NULL;
    }

    /* Itero en los LSA que forman parte del LSU. Para cada uno, actualizo la topología.*/
    Debug("-> PWOSPF: Processing LSAs and updating topology table\n");

    unsigned int i = 0;
    for (i; i < rx_ospfv2_lsu_hdr->num_adv; i++)
    {
        rx_ospfv2_lsa = ((ospfv2_lsa_t *)(rx_lsu_param->packet + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t) + sizeof(ospfv2_hdr_t) +
                                          sizeof(ospfv2_lsu_hdr_t) + (sizeof(ospfv2_lsa_t) * i)));

        struct in_addr router_id;
        router_id.s_addr = rx_ospfv2_hdr->rid;
        struct in_addr net_num;
        /* Obtengo subnet */
        net_num.s_addr = rx_ospfv2_lsa->subnet;
        struct in_addr net_mask;    
        net_mask.s_addr = rx_ospfv2_lsa->mask;
        struct in_addr neighbor_id;
        /* Obtengo vecino */
        neighbor_id.s_addr = rx_ospfv2_lsa->rid;
        struct in_addr ip_src;
        ip_src.s_addr = ip_hdr->ip_src;
        Debug("      [Subnet = %s]", inet_ntoa(net_num));
        Debug("      [Mask = %s]", inet_ntoa(net_mask));
        Debug("      [Neighbor ID = %s]\n", inet_ntoa(neighbor_id));
        /* LLamo a refresh_topology_entry*/
        refresh_topology_entry(g_topology, router_id, net_num, net_mask, neighbor_id, ip_src, rx_ospfv2_lsu_hdr->seq);
    }

    /* Imprimo la topología */
    Debug("\n-> PWOSPF: Printing the topology table\n");
 
    /* Ejecuto Dijkstra en un nuevo hilo (run_dijkstra)*/
    /* Running Dijkstra thread */
    Debug("\n-> PWOSPF: Running the Dijkstra algorithm\n\n");
    dijkstra_param_t *dij_param = ((dijkstra_param_t *)(malloc(sizeof(dijkstra_param_t))));
    dij_param->sr = rx_lsu_param->sr;
    dij_param->topology = g_topology;
    dij_param->rid = g_router_id;
    dij_param->mutex = g_dijkstra_mutex;
    pthread_create(&g_dijkstra_thread, NULL, run_dijkstra, dij_param);

    /* Flooding del LSU por todas las interfaces menos por donde me llegó */
    struct sr_if *temp_int = rx_lsu_param->sr->if_list;

    if(rx_ospfv2_lsu_hdr->ttl < 1){
        return;
    }
    else
    {
        rx_ospfv2_lsu_hdr->ttl--;
        rx_ospfv2_hdr->csum = 0;
        rx_ospfv2_hdr->csum = ospfv2_cksum(rx_ospfv2_hdr , rx_lsu_param->length -(sizeof(sr_ethernet_hdr_t) - sizeof(sr_ip_hdr_t)));
        while (temp_int != NULL)
        {
            if ((strcmp(temp_int->name, rx_lsu_param->rx_if->name) != 0 && (temp_int->neighbor_id)))
            {
                /* Seteo MAC de origen */
                memcpy(((sr_ethernet_hdr_t *)(rx_lsu_param->packet))->ether_shost, temp_int->addr , ETHER_ADDR_LEN);
                

                /* Ajusto paquete IP, origen y checksum*/
                ((sr_ip_hdr_t *)(rx_lsu_param->packet + sizeof(sr_ethernet_hdr_t)))->ip_dst = temp_int->neighbor_ip;

                /* IP Checksum */
                ((sr_ip_hdr_t *)(rx_lsu_param->packet + sizeof(sr_ethernet_hdr_t)))->ip_sum = 0;

                /* Source IP address */
                ((sr_ip_hdr_t *)(rx_lsu_param->packet + sizeof(sr_ethernet_hdr_t)))->ip_src = temp_int->ip;

                /* Re-Calculate checksum of the IP header */
                ((sr_ip_hdr_t *)(rx_lsu_param->packet + sizeof(sr_ethernet_hdr_t)))->ip_sum = ip_cksum(((uint8_t *)(rx_lsu_param->packet + sizeof(sr_ethernet_hdr_t))),
                                                                                                    sizeof(sr_ip_hdr_t));

                
                struct sr_arpentry *arpEntry = sr_arpcache_lookup(&(rx_lsu_param->sr->cache), temp_int->neighbor_ip);
                if (arpEntry != NULL)
                    {
                        /* Si la entrada existe, usar la dirección MAC de arpEntry */
                        memcpy(((sr_ethernet_hdr_t *)(rx_lsu_param->packet))->ether_dhost, arpEntry->mac, ETHER_ADDR_LEN);
                        sr_send_packet(rx_lsu_param->sr, rx_lsu_param->packet, rx_lsu_param->length, temp_int->name);
                        /* Liberar la entrada ARP obtenida */
                        free(arpEntry);
                    }
                    else
                    {
                        /* Si no hay entrada ARP, encolar la solicitud ARP*/
                        struct sr_arpreq *req = sr_arpcache_queuereq(&(rx_lsu_param->sr->cache), temp_int->neighbor_ip, rx_lsu_param->packet, rx_lsu_param->length, temp_int->name);
                        handle_arpreq(rx_lsu_param->sr, req); /* Maneja la solicitud ARP, enviará el ARP request si es necesario */
                    }
            }

            temp_int = temp_int->next;
        }
    }

    return NULL;
} /* -- sr_handle_pwospf_lsu_packet -- */

/**********************************************************************************
 * SU CÓDIGO DEBERÍA TERMINAR AQUÍ
 * *********************************************************************************/

/*---------------------------------------------------------------------
 * Method: sr_handle_pwospf_packet
 *
 * Gestiona los paquetes PWOSPF
 *
 *---------------------------------------------------------------------*/

void sr_handle_pwospf_packet(struct sr_instance *sr, uint8_t *packet, unsigned int length, struct sr_if *rx_if)
{
    /*Si aún no terminó la inicialización, se descarta el paquete recibido*/
    if (g_router_id.s_addr == 0)
    {
        return;
    }

    ospfv2_hdr_t *rx_ospfv2_hdr = ((ospfv2_hdr_t *)(packet + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t)));
    powspf_rx_lsu_param_t *rx_lsu_param = ((powspf_rx_lsu_param_t *)(malloc(sizeof(powspf_rx_lsu_param_t))));

    Debug("-> PWOSPF: Detecting PWOSPF Packet\n");
    Debug("      [Type = %d]\n", rx_ospfv2_hdr->type);

    switch (rx_ospfv2_hdr->type)
    {
    case OSPF_TYPE_HELLO:
        sr_handle_pwospf_hello_packet(sr, packet, length, rx_if);
        break;
    case OSPF_TYPE_LSU:
        rx_lsu_param->sr = sr;
        unsigned int i;
        for (i = 0; i < length; i++)
        {
            rx_lsu_param->packet[i] = packet[i];
        }
        rx_lsu_param->length = length;
        rx_lsu_param->rx_if = rx_if;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_t pid;
        pthread_create(&pid, &attr, sr_handle_pwospf_lsu_packet, rx_lsu_param);
        break;
    }
} /* -- sr_handle_pwospf_packet -- */
