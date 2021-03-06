/*
 * dnspod+域名解析服务
 * dnspod: https://www.dnspod.cn/
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>

#ifdef __WIN32__
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/select.h>
#endif  /* __WIN32__ */

#ifndef NULL
#define NULL 0
#endif

#include "dplus.h"
#include "http.c"
#include "des.c"
#include "key.c"
#include "cache.c"

#define HTTPDNS_DEFAULT_SERVER "119.29.29.29"
#define HTTPDNS_DEFAULT_PORT   80
#define HTTP_DEFAULT_DATA_SIZE 1024


struct host_info {
    int h_addrtype; /* host address type: AF_INET or AF_INET6 */
    int h_length;   /* length of address in bytes: 
                       sizeof(struct in_addr) or sizeof(struct in6_addr) */
    int addr_list_len; /* length of addr list */
    char **h_addr_list; /* list of addresses */
};

static int 
strchr_num(const char *str, char c) {
    int count = 0;
    while (*str){
        if (*str++ == c){
            count++;
        }
    }
    return count;
}

static int 
is_address(const char *s) {
    unsigned char buf[sizeof(struct in6_addr)];
    int r;
    r = inet_pton(AF_INET, s, buf);
    if (r <= 0) {
        r = inet_pton(AF_INET6, s, buf);
        return (r > 0);
    }
    return 1;
}

static int 
is_integer(const char *s) {
    if (*s == '-' || *s == '+')
        s++;
    if (*s < '0' || '9' < *s)
        return 0;
    s++;
    while ('0' <= *s && *s <= '9')
        s++;
    return (*s == '\0');
}

static void 
host_info_clear(struct host_info *host) {
    int i;
    for (i = 0; i < host->addr_list_len; i++) {
        if (host->h_addr_list[i]) {
            free(host->h_addr_list[i]);
        }
    }
    free(host->h_addr_list);
    free(host);
}

static struct host_info *
http_query(const char *node, time_t *ttl) {
    int i, ret, sockfd;
    struct host_info *hi;
    char http_data[HTTP_DEFAULT_DATA_SIZE + 1];
    char *http_data_ptr, *http_data_ptr_head;
    char *comma_ptr;
    char * node_en;

    sockfd = make_connection(HTTPDNS_DEFAULT_SERVER, HTTPDNS_DEFAULT_PORT);
    if (sockfd < 0) {
        return NULL;
    }

    if (des_used) {
        node_en = des_encode_hex((char *)node, strlen(node), des_key); 
        snprintf(http_data, HTTP_DEFAULT_DATA_SIZE, "/d?dn=%s&ttl=1&id=%d", node_en, des_id);
        free(node_en);
    }
    else
        snprintf(http_data, HTTP_DEFAULT_DATA_SIZE, "/d?dn=%s&ttl=1", node);
    http_data[HTTP_DEFAULT_DATA_SIZE] = 0;

    ret = make_request(sockfd, HTTPDNS_DEFAULT_SERVER, http_data);
    if (ret < 0){
#ifdef __WIN32__
        closesocket(sockfd);
#else
        close(sockfd);
#endif
        return NULL;
    }

    ret = fetch_response(sockfd, http_data, HTTP_DEFAULT_DATA_SIZE);
#ifdef __WIN32__
    closesocket(sockfd);
#else
    close(sockfd);
#endif

    if (ret < 0) {
        return NULL;
    }

    if (des_used) {
        http_data_ptr = des_decode_hex(http_data, des_key, NULL); 
        if (NULL == http_data_ptr) {
            return NULL;
        }
        http_data_ptr_head = http_data_ptr;
        _DPLUS_DEBUG("http_data_ptr: %s\n", http_data_ptr);
    }
    else {
        http_data_ptr = http_data;
    }

    comma_ptr = strchr(http_data_ptr, ',');
    if (comma_ptr != NULL) {
        sscanf(comma_ptr + 1, "%ld", ttl);
        *comma_ptr = '\0';
    }
    else {
        *ttl = 0;
    }

    hi = (struct host_info *)malloc(sizeof(struct host_info));
    if (hi == NULL) {
        fprintf(stderr, "malloc struct host_info failed\n");
        if(des_used) {
            free(http_data_ptr_head);
        }
        return NULL;
    }

    /* Only support IPV4 */
    hi->h_addrtype = AF_INET;
    hi->h_length = sizeof(struct in_addr);
    hi->addr_list_len = strchr_num(http_data_ptr, ';') + 1;
    hi->h_addr_list = (char **)calloc(hi->addr_list_len, sizeof(char *));
    if (hi->h_addr_list == NULL) {
        fprintf(stderr, "calloc addr_list failed\n");
        free(hi);
        goto error;
    }

    memset(hi->h_addr_list, 0x00, hi->addr_list_len * sizeof(char *));
    for (i = 0; i < hi->addr_list_len; ++i) {
        char *addr;
        char *ipstr = http_data_ptr;
        char *semicolon = strchr(ipstr, ';');
        if (semicolon != NULL) {
            *semicolon = '\0';
            http_data_ptr = semicolon + 1;
        }

        addr = (char *)malloc(sizeof(struct in_addr));
        if (addr == NULL) {
            fprintf(stderr, "malloc struct in_addr failed\n");
            host_info_clear(hi);
            goto error;
        }
        ret = inet_pton(AF_INET, ipstr, addr);
        if (ret <= 0) {
            fprintf(stderr, "invalid ipstr:%s\n", ipstr);
            free(addr);
            host_info_clear(hi);
            goto error;
        }

        hi->h_addr_list[i] = addr;
    }

    if (des_used)
        free(http_data_ptr_head);

    return hi;

error:
    if (des_used)
        free(http_data_ptr_head);

    return NULL;
}

static struct addrinfo *
malloc_addrinfo(int port, uint32_t addr, int socktype, int proto) {
    struct addrinfo *ai;
    struct sockaddr_in *sa_in;
    size_t socklen;
    socklen = sizeof(struct sockaddr);
    
    ai = (struct addrinfo *)calloc(1, sizeof(struct addrinfo));
    if (!ai)
        return NULL;
    
    ai->ai_socktype = socktype;
    ai->ai_protocol = proto;
    
    ai->ai_addr = (struct sockaddr *)calloc(1, sizeof(struct sockaddr));
    if (!ai->ai_addr) {
        free(ai);
        return NULL;
    };
    
    ai->ai_addrlen = socklen;
    ai->ai_addr->sa_family = ai->ai_family = AF_INET;
    
    sa_in = (struct sockaddr_in *)ai->ai_addr;
    sa_in->sin_port = port;
    sa_in->sin_addr.s_addr = addr;
    
    return ai;
}

void
dp_freeaddrinfo(struct addrinfo *ai) {
    struct addrinfo *next;
    while (ai != NULL) {
        if (ai->ai_canonname != NULL)
            free(ai->ai_canonname);
        if (ai->ai_addr)
            free(ai->ai_addr);
        next = ai->ai_next;
        free(ai);
        ai = next;
    }
}

static struct addrinfo *
dup_addrinfo(struct addrinfo *ai) {
    struct addrinfo *cur, *head = NULL, *prev = NULL;
    while (ai != NULL) {
        cur = (struct addrinfo *)malloc(sizeof(struct addrinfo));
        if (!cur)
            goto error;

        memcpy(cur, ai, sizeof(struct addrinfo));

        cur->ai_addr = (struct sockaddr *)malloc(sizeof(struct sockaddr));
        if (!cur->ai_addr) {
            free(cur);
            goto error;
        };
        memcpy(cur->ai_addr, ai->ai_addr, sizeof(struct sockaddr));

        if (ai->ai_canonname)
            cur->ai_canonname = strdup(ai->ai_canonname);

        if (prev)
            prev->ai_next = cur;
        else
            head = cur;
        prev = cur;

        ai = ai->ai_next;
    }

    return head;

error:
    if (head) {
        dp_freeaddrinfo(head);
    }
    return NULL;
}

static int 
fillin_addrinfo_res(struct addrinfo **res, struct host_info *hi,
    int port, int socktype, int proto) {
    int i;
    struct addrinfo *cur, *prev = NULL;
    for (i = 0; i < hi->addr_list_len; i++) {
        struct in_addr *in = ((struct in_addr *)hi->h_addr_list[i]);
        cur = malloc_addrinfo(port, in->s_addr, socktype, proto);
        if (cur == NULL) {
            if (*res)
                dp_freeaddrinfo(*res);
            return EAI_MEMORY;
        }
        if (prev)
            prev->ai_next = cur;
        else
            *res = cur;
        prev = cur;
    }
    return 0;
}

int
dp_getaddrinfo(const char *node, const char *service, \
        const struct addrinfo *hints, struct addrinfo **res) {
    struct host_info *hi = NULL;
    int port = 0, socktype, proto, ret = 0;
    time_t ttl, rawtime;
    struct addrinfo *answer;
    struct cache_data * c_data;
    #ifdef __WIN32__
        WSADATA wsa;
    #endif

    if (node == NULL)
        return EAI_NONAME;

    #ifdef __WIN32__
        WSAStartup(MAKEWORD(2, 2), &wsa);
    #endif

    _DPLUS_INFO("!!!! node = %s\n", node);
    *res = NULL;
    
    if (is_address(node) || (hints && (hints->ai_flags & AI_NUMERICHOST)))
        goto SYS_DNS;

    if (hints && hints->ai_family != PF_INET
        && hints->ai_family != PF_UNSPEC
        && hints->ai_family != PF_INET6) {
        goto SYS_DNS;
    }
    if (hints && hints->ai_socktype != SOCK_DGRAM
        && hints->ai_socktype != SOCK_STREAM
        && hints->ai_socktype != 0) {
        goto SYS_DNS;
    }

    socktype = (hints && hints->ai_socktype) ? hints->ai_socktype : SOCK_STREAM;
    if (hints && hints->ai_protocol)
        proto = hints->ai_protocol;
    else {
        switch (socktype) {
        case SOCK_DGRAM:
            proto = IPPROTO_UDP;
            break;
        case SOCK_STREAM:
            proto = IPPROTO_TCP;
            break;
        default:
            proto = 0;
            break;
        }
    }

    if (service != NULL && service[0] == '*' && service[1] == 0)
        service = NULL;

    if (service != NULL) {
        if (is_integer(service))
            port = htons(atoi(service));
        else {
            struct servent *servent;
            char *pe_proto;
            switch (socktype){
            case SOCK_DGRAM:
                pe_proto = "udp";
                break;
            case SOCK_STREAM:
                pe_proto = "tcp";
                break;
            default:
                pe_proto = "tcp";
                break;
            }
            servent = getservbyname(service, pe_proto);
            if (servent == NULL) {
                goto SYS_DNS;
            }
            port = servent->s_port;
        }
    }
    else {
        port = htons(0);
    }

    cache_lock();

    c_data = cache_get((char *)node, ntohs(port));
    if(c_data) {
        hi = c_data->hi;
        time(&rawtime);
        if(c_data->expire_time > rawtime) {
            ret = fillin_addrinfo_res(res, hi, port, socktype, proto);
            _DPLUS_DEBUG("CACHE_DNS: ret = %d, node = %s\n", ret, node);
            cache_unlock();
            if(ret == 0) goto RET;
            else goto SYS_DNS; 
        }
        cache_remove((char *)node, ntohs(port));
    }

    /*
    * 首先使用HttpDNS向D+服务器进行请求,
    * 如果失败则调用系统接口进行解析，该结果不会缓存
    */
    hi = http_query(node, &ttl);
    if (NULL == hi) {
        _DPLUS_INFO("!!! HTTP_DNS FAILED.\n");
        cache_unlock();
        goto SYS_DNS;
    }

    ret = fillin_addrinfo_res(res, hi, port, socktype, proto);
    _DPLUS_DEBUG("HTTP_DNS: ret = %d, node = %s\n", ret, node);

    /* 缓存时间 3/4*ttl分钟 */
    if(ret != 0 || cache_set((char *)node, ntohs(port), hi, ttl*60/4*3) != 0) {
        host_info_clear(hi);
    }
    cache_unlock();

    if(ret == 0) goto RET;

SYS_DNS:
    *res = NULL;
    ret = getaddrinfo(node, service, hints, &answer);
    _DPLUS_DEBUG("SYS_DNS: ret = %d, node = %s\n", ret, node);
    if (ret == 0) {
        *res = dup_addrinfo(answer);
        freeaddrinfo(answer);
        if (*res == NULL) {
            return EAI_MEMORY;
        }
    }

RET:
    #ifdef __WIN32__
        WSACleanup();
    #endif
    return ret;
}

void dp_cache_clear() {
    cache_lock();
    cache_clear();
    cache_unlock();
}

#include "dplus_wide.c"

