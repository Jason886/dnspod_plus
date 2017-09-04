/*
 * dnspod域名解析服务
 * reflink: https://www.dnspod.cn/
 */


#include <sys/socket.h> 
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void des_ecb_pkcs5(char * in, size_t size_in, char key[8], char ** out, size_t *size_out, char mode);

static char * _dns_server = "119.29.29.29";
static int _dns_port = 80;
static char *key = "chivox.com";
static int key_id = 0;

static int _connect() {
    struct in_addr inaddr;
    struct sockaddr_in addr;
    int sock;

    inaddr.s_addr = inet_addr(_dns_server);
    memset(&addr, 0x00, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(_dns_port);
    addr.sin_addr = inaddr;

    sock = socket(PF_INET, SOCK_STREAM, 0);
    if(sock < 0) {
        return -1;
    }

    if(connect(sock, (const struct sockaddr *)&addr, sizeof(addr)) != 0) {
        return -1;
    }

    return sock;
}

static void free_ips(char **ips, int count) {
    int i = 0;
    for(i=0; i< count; i++) {
        char * ip = (char *)(((char **)(ips))[i]);
        free(ip);
    }
    free(ips);
}

static void hex_2_bin(char *hex, size_t size, char *bin) {
    size_t i;
    char a, b;
    for(i=0; i < size; i+=2) {
        a = hex[i];
        a = (a >= '0' && a <= '9') ? (a - '0') : 
            (
             (a >= 'A' && a <= 'F') ? (a - 'A' + 10) : (a - 'a' + 10)
            );
        b = hex[i+1];
        b = (b >= '0' && b <= '9') ? (b - '0') : 
            (
             (b >= 'A' && b <= 'F') ? (b - 'A' + 10) : (b - 'a' + 10)
            );
        bin[i/2] = (a<<4) + b;
    }
}

static void bin_2_hex(char *bin, size_t size, char *hex, int upper) {
    size_t i;
    char a;
    for(i=0; i < size; i++) {
        a = (bin[i] >> 4) & 0x0F;
        hex[i*2] = (a >= 0 && a <= 9) ? (a + '0') :
            (upper ? (a - 10 + 'A') : (a - 10 + 'a'));

        a = (bin[i]) & 0x0F;
        hex[i*2+1] = (a >= 0 && a <= 9) ? (a + '0') :
            (upper ? (a - 10 + 'A') : (a - 10 + 'a'));
    }
}

static void parse_data(char *data, char * key, int *count, char ***ips, int *ttl) {
    const int STATUS = 0;
    const int HEADER = 1;
    const int BODY = 2;
    int state = STATUS;
    char * line = 0;
    char * body = 0;
    char * body_bin = 0;
    char * body_de = 0;
    size_t body_de_size = 0;
    char * ip_str = 0;
    char * ttl_str = 0;
    char * tok = 0;
    char * str_dup = 0;
    void * tmp = 0;
    int i;

    if(count) *count = 0;
    if(ips) *ips = 0;
    if(ttl) *ttl = 0;

    line = strtok(data, "\n");
    while(line != 0) {
        if(state == STATUS) {
            if(strstr(line, "200 ") == 0) {
                return;
            }
            state = HEADER;
            goto next;
        }

        if(state == HEADER) {
            if(strstr(line, ":") != 0) {
                goto next;
            }
            state = BODY;
        }

        if(state == BODY) {
            body = line + strlen(line) +1;
            break;
        }

next:
        line = strtok(0, "\n");
    }

    if(!body || strlen(body) == 0) {
        return;
    }

    if(key) {
        body_bin = malloc(strlen(body)/2+1);
        if(!body_bin) return;
        hex_2_bin(body, strlen(body)/2+1, body_bin);
        des_ecb_pkcs5(body_bin, strlen(body)/2, key, &body_de, &body_de_size, 'd');
        free(body_bin);
        body_de[body_de_size] = 0;
        body = body_de;
    }

    printf("body = %s\n", body);

    ip_str = strtok(body, ",");
    printf("ip_str = %s\n", ip_str);
    ttl_str = strtok(0, ",");
    printf("ttl_str = %s\n", ttl_str);

    if(ip_str) {
        tok = strtok(ip_str, ";");
        printf("tok = %s\n", tok);
        while(tok != 0) {
            if(count) {
                (*count)++;
                
                if(*count == 1) {
                    *ips = malloc((*count)*sizeof(char *));
                    if(!(*ips))
                    {
                        *count = 0;
                        goto next_tok;
                    }
                }
                else {
                    printf("realloc\n");
                    tmp = realloc(*ips, (*count)*sizeof(char *));
                    printf("tmp = %p\n", tmp);
                    if(!tmp) {
                        printf("in it\n");
                        (*count)--;
                        goto next_tok;
                    }
                    printf("out it\n");
                    *ips = tmp;
                }
                str_dup = malloc(strlen(tok) +1);
                strcpy(str_dup, tok);
                ((char **)(*ips))[(*count)-1] = str_dup;
                
            }
next_tok:
            tok = strtok(0, ";");
            printf("tok = %s\n", tok);
        }

        printf("*count = %d\n", *count);

        for(i=0; i < *count; i++) {
            char * ip = (char *)(((char **)(*ips))[i]);
            printf("ip = %s\n", ip);
        }
    }

    if(body_de) free(body_de);
}

#define IP_OUT_SIZE 32
int dns_pod(char * dn, char * local_ip, int encrypt, int key_id, char * key, char *ip_out/*size=32*/, int *ttl) {
    int sock = -1;
    char *fmt;
    char *dn_en = 0;
    char *dn_en_hex = 0;
    size_t dn_en_size = 0;
    char buff[4096];
    char * data = 0;
    int total, nsend, sended, nread, readed;
    int count;
    char **ips = 0;
    int ttl_rsp;
    int ret = -1;
    char *tmp = 0;

    sock = _connect();
    if(sock < 0) {
        return -1;
    }

    if(local_ip && strlen(local_ip) > 0) {
        if(ttl) {
            if(encrypt) {
                fmt = "GET /d?dn=%s&ip=%s&id=%d&ttl=1 HTTP/1.1\r\n" "\r\n";
            }
            else {
                fmt = "GET /d?dn=%s&ip=%s&ttl=1 HTTP/1.1\r\n" "\r\n";
            }
        }
        else {
            if(encrypt) {
                fmt = "GET /d?dn=%s&ip=%s&id=%d HTTP/1.1\r\n" "\r\n";
            }
            else {
                fmt = "GET /d?dn=%s&ip=%s HTTP/1.1\r\n" "\r\n";
            }
        }
        if(encrypt) {
            des_ecb_pkcs5(dn, strlen(dn), key, &dn_en, &dn_en_size, 'e');
            if(!dn_en) goto ERR;
            dn_en_hex = malloc(dn_en_size*2+1);
            if(!dn_en_hex) goto ERR;
            memset(dn_en_hex, 0x00, dn_en_size*2+1); 
            bin_2_hex(dn_en, dn_en_size, dn_en_hex, 1); 
            snprintf((char *)buff, sizeof(buff), (const char *)fmt, dn_en_hex, local_ip, key_id);
        }
        else {
            snprintf(buff, sizeof(buff), fmt, dn, local_ip);
        }
    }
    else {
        if(ttl) {
            if(encrypt) {
                fmt = "GET /d?dn=%s&id=%d&ttl=1 HTTP/1.1\r\n" "\r\n";
            }
            else {
                fmt = "GET /d?dn=%s&ttl=1 HTTP/1.1\r\n" "\r\n";
            }
        }
        else {
            if(encrypt) {
                fmt = "GET /d?dn=%s&id=%d HTTP/1.1\r\n" "\r\n";
            }
            else {
                fmt = "GET /d?dn=%s HTTP/1.1\r\n" "\r\n";
            }
        }
        if(encrypt) {
            des_ecb_pkcs5(dn, strlen(dn), key, &dn_en, &dn_en_size, 'e');
            if(!dn_en) goto ERR;
            dn_en_hex = malloc(dn_en_size*2+1);
            if(!dn_en_hex) goto ERR;
            memset(dn_en_hex, 0x00, dn_en_size*2+1); 
            bin_2_hex(dn_en, dn_en_size, dn_en_hex, 1); 
            snprintf(buff, sizeof(buff), fmt, dn_en_hex, key_id);
        }
        else {
            snprintf(buff, sizeof(buff), fmt, dn);
        }
    }

    printf("send: %s\n", buff);

    total = strlen(buff);
    sended = 0;
    while(sended < total) {
        nsend = send(sock, buff+sended, total-sended, 0);
        if(nsend < 0) {
            goto ERR;
        }
        sended +=nsend;
    }

    readed = 0;
    data = 0;
    while(1) {
        memset(buff, 0x00, sizeof(buff));
        nread = recv(sock, buff, sizeof(buff), 0);
        if(nread <= 0) {
            break;
        }
        if(data) {
            tmp = realloc(data, readed + nread); 
            if(!tmp) goto ERR;
            data = tmp;
        }
        else {
            data = malloc(nread);
            if(!data) goto ERR;
        }
        memcpy(data+readed, buff, nread);
        readed +=nread;
    }

    printf("recv data = %s\n, readed = %d\n", data, readed);
   
    if(encrypt) {
        parse_data(data, key, &count, &ips, &ttl_rsp);
    }
    else {
        parse_data(data, 0,  &count, &ips, &ttl_rsp);
    }

    memset(ip_out, 0x00, IP_OUT_SIZE);
    if(ips && count > 0) {
       strncpy(ip_out, (char *)(((char**)(ips))[0]), IP_OUT_SIZE-1);
    }
    *ttl = ttl_rsp;

    free_ips(ips, count);

    ret = 0;
ERR:
    close(sock);
    if(dn_en) free(dn_en);
    if(dn_en_hex) free(dn_en_hex);
    if(data) free(data);
    return ret;
}

int main() {
    int ttl;
    char ip[32];
    char * test_hex = "ABCDEF0123456789abcdef";
    char bin[40];
    char hex[100];
    int i;
    dns_pod("www.baidu.com", 0, 0, key_id, key, ip, &ttl);
    printf("ip = %s\n", ip);
    memset(bin, 0x00, sizeof(bin));
    hex_2_bin(test_hex, strlen(test_hex), bin);
    for(i=0; i<40; i++) {
        printf("%02X ", (uint8_t) bin[i]);
    }
    printf("\n");
    memset(hex, 0x00, sizeof(hex));
    bin_2_hex(bin, 11, hex, 1);
    hex[22] = 0;
    printf("hex = %s\n", hex);
    memset(hex, 0x00, sizeof(hex));
    bin_2_hex(bin, 11, hex, 0);
    hex[22] = 0;
    printf("hex = %s\n", hex);
    
    
    return 0;
}


/* =================================================================================== */
/*                             DES                                                     */
/* ----------------------------------------------------------------------------------- */
/*
 * Data Encryption Standard
 * An approach to DES algorithm
 * 
 * By: Daniel Huertas Gonzalez
 * Email: huertas.dani@gmail.com
 * Version: 0.1
 * 
 * Based on the document FIPS PUB 46-3
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define LB32_MASK   0x00000001
#define LB64_MASK   0x0000000000000001
#define L64_MASK    0x00000000ffffffff
#define H64_MASK    0xffffffff00000000

/* Initial Permutation Table */
static char IP[] = {
    58, 50, 42, 34, 26, 18, 10,  2, 
    60, 52, 44, 36, 28, 20, 12,  4, 
    62, 54, 46, 38, 30, 22, 14,  6, 
    64, 56, 48, 40, 32, 24, 16,  8, 
    57, 49, 41, 33, 25, 17,  9,  1, 
    59, 51, 43, 35, 27, 19, 11,  3, 
    61, 53, 45, 37, 29, 21, 13,  5, 
    63, 55, 47, 39, 31, 23, 15,  7
};

/* Inverse Initial Permutation Table */
static char PI[] = {
    40,  8, 48, 16, 56, 24, 64, 32, 
    39,  7, 47, 15, 55, 23, 63, 31, 
    38,  6, 46, 14, 54, 22, 62, 30, 
    37,  5, 45, 13, 53, 21, 61, 29, 
    36,  4, 44, 12, 52, 20, 60, 28, 
    35,  3, 43, 11, 51, 19, 59, 27, 
    34,  2, 42, 10, 50, 18, 58, 26, 
    33,  1, 41,  9, 49, 17, 57, 25
};

/*Expansion table */
static char E[] = {
    32,  1,  2,  3,  4,  5,  
     4,  5,  6,  7,  8,  9,  
     8,  9, 10, 11, 12, 13, 
    12, 13, 14, 15, 16, 17, 
    16, 17, 18, 19, 20, 21, 
    20, 21, 22, 23, 24, 25, 
    24, 25, 26, 27, 28, 29, 
    28, 29, 30, 31, 32,  1
};

/* Post S-Box permutation */
static char P[] = {
    16,  7, 20, 21, 
    29, 12, 28, 17, 
     1, 15, 23, 26, 
     5, 18, 31, 10, 
     2,  8, 24, 14, 
    32, 27,  3,  9, 
    19, 13, 30,  6, 
    22, 11,  4, 25
};

/* The S-Box tables */
static char S[8][64] = {{
    /* S1 */
    14,  4, 13,  1,  2, 15, 11,  8,  3, 10,  6, 12,  5,  9,  0,  7,  
     0, 15,  7,  4, 14,  2, 13,  1, 10,  6, 12, 11,  9,  5,  3,  8,  
     4,  1, 14,  8, 13,  6,  2, 11, 15, 12,  9,  7,  3, 10,  5,  0, 
    15, 12,  8,  2,  4,  9,  1,  7,  5, 11,  3, 14, 10,  0,  6, 13
},{
    /* S2 */
    15,  1,  8, 14,  6, 11,  3,  4,  9,  7,  2, 13, 12,  0,  5, 10,  
     3, 13,  4,  7, 15,  2,  8, 14, 12,  0,  1, 10,  6,  9, 11,  5,  
     0, 14,  7, 11, 10,  4, 13,  1,  5,  8, 12,  6,  9,  3,  2, 15, 
    13,  8, 10,  1,  3, 15,  4,  2, 11,  6,  7, 12,  0,  5, 14,  9
},{
    /* S3 */
    10,  0,  9, 14,  6,  3, 15,  5,  1, 13, 12,  7, 11,  4,  2,  8,  
    13,  7,  0,  9,  3,  4,  6, 10,  2,  8,  5, 14, 12, 11, 15,  1,  
    13,  6,  4,  9,  8, 15,  3,  0, 11,  1,  2, 12,  5, 10, 14,  7,
     1, 10, 13,  0,  6,  9,  8,  7,  4, 15, 14,  3, 11,  5,  2, 12
},{
    /* S4 */
     7, 13, 14,  3,  0,  6,  9, 10,  1,  2,  8,  5, 11, 12,  4, 15,  
    13,  8, 11,  5,  6, 15,  0,  3,  4,  7,  2, 12,  1, 10, 14,  9,  
    10,  6,  9,  0, 12, 11,  7, 13, 15,  1,  3, 14,  5,  2,  8,  4,
     3, 15,  0,  6, 10,  1, 13,  8,  9,  4,  5, 11, 12,  7,  2, 14
},{
    /* S5 */
     2, 12,  4,  1,  7, 10, 11,  6,  8,  5,  3, 15, 13,  0, 14,  9, 
    14, 11,  2, 12,  4,  7, 13,  1,  5,  0, 15, 10,  3,  9,  8,  6, 
     4,  2,  1, 11, 10, 13,  7,  8, 15,  9, 12,  5,  6,  3,  0, 14, 
    11,  8, 12,  7,  1, 14,  2, 13,  6, 15,  0,  9, 10,  4,  5,  3
},{
    /* S6 */
    12,  1, 10, 15,  9,  2,  6,  8,  0, 13,  3,  4, 14,  7,  5, 11,
    10, 15,  4,  2,  7, 12,  9,  5,  6,  1, 13, 14,  0, 11,  3,  8,
     9, 14, 15,  5,  2,  8, 12,  3,  7,  0,  4, 10,  1, 13, 11,  6,
     4,  3,  2, 12,  9,  5, 15, 10, 11, 14,  1,  7,  6,  0,  8, 13
},{
    /* S7 */
     4, 11,  2, 14, 15,  0,  8, 13,  3, 12,  9,  7,  5, 10,  6,  1,
    13,  0, 11,  7,  4,  9,  1, 10, 14,  3,  5, 12,  2, 15,  8,  6,
     1,  4, 11, 13, 12,  3,  7, 14, 10, 15,  6,  8,  0,  5,  9,  2,
     6, 11, 13,  8,  1,  4, 10,  7,  9,  5,  0, 15, 14,  2,  3, 12
},{
    /* S8 */
    13,  2,  8,  4,  6, 15, 11,  1, 10,  9,  3, 14,  5,  0, 12,  7,
     1, 15, 13,  8, 10,  3,  7,  4, 12,  5,  6, 11,  0, 14,  9,  2,
     7, 11,  4,  1,  9, 12, 14,  2,  0,  6, 10, 13, 15,  3,  5,  8,
     2,  1, 14,  7,  4, 10,  8, 13, 15, 12,  9,  0,  3,  5,  6, 11
}};

/* Permuted Choice 1 Table */
static char PC1[] = {
    57, 49, 41, 33, 25, 17,  9,
     1, 58, 50, 42, 34, 26, 18,
    10,  2, 59, 51, 43, 35, 27,
    19, 11,  3, 60, 52, 44, 36,
    
    63, 55, 47, 39, 31, 23, 15,
     7, 62, 54, 46, 38, 30, 22,
    14,  6, 61, 53, 45, 37, 29,
    21, 13,  5, 28, 20, 12,  4
};

/* Permuted Choice 2 Table */
static char PC2[] = {
    14, 17, 11, 24,  1,  5,
     3, 28, 15,  6, 21, 10,
    23, 19, 12,  4, 26,  8,
    16,  7, 27, 20, 13,  2,
    41, 52, 31, 37, 47, 55,
    30, 40, 51, 45, 33, 48,
    44, 49, 39, 56, 34, 53,
    46, 42, 50, 36, 29, 32
};

/* Iteration Shift Array */
static char iteration_shift[] = {
 /* 1   2   3   4   5   6   7   8   9  10  11  12  13  14  15  16 */
    1,  1,  2,  2,  2,  2,  2,  2,  1,  2,  2,  2,  2,  2,  2,  1
};

/*
 * The DES function
 * input: 64 bit message
 * key: 64 bit key for encryption/decryption
 * mode: 'e' = encryption; 'd' = decryption
 */
static uint64_t _des_uint64(uint64_t input, uint64_t key, char mode) {
    
    int i, j;
    
    /* 8 bits */
    char row, column;
    
    /* 28 bits */
    uint32_t C                  = 0;
    uint32_t D                  = 0;
    
    /* 32 bits */
    uint32_t L                  = 0;
    uint32_t R                  = 0;
    uint32_t s_output           = 0;
    uint32_t f_function_res     = 0;
    uint32_t temp               = 0;
    
    /* 48 bits */
    uint64_t sub_key[16]        = {0};
    uint64_t s_input            = 0;
    
    /* 56 bits */
    uint64_t permuted_choice_1  = 0;
    uint64_t permuted_choice_2  = 0;
    
    /* 64 bits */
    uint64_t init_perm_res      = 0;
    uint64_t inv_init_perm_res  = 0;
    uint64_t pre_output         = 0;
    
    /* initial permutation */
    for (i = 0; i < 64; i++) {
        
        init_perm_res <<= 1;
        init_perm_res |= (input >> (64-IP[i])) & LB64_MASK;
        
    }
    
    L = (uint32_t) (init_perm_res >> 32) & L64_MASK;
    R = (uint32_t) init_perm_res & L64_MASK;
        
    /* initial key schedule calculation */
    for (i = 0; i < 56; i++) {
        
        permuted_choice_1 <<= 1;
        permuted_choice_1 |= (key >> (64-PC1[i])) & LB64_MASK;

    }
    
    C = (uint32_t) ((permuted_choice_1 >> 28) & 0x000000000fffffff);
    D = (uint32_t) (permuted_choice_1 & 0x000000000fffffff);
    
    /* Calculation of the 16 keys */
    for (i = 0; i< 16; i++) {
        
        /* key schedule */
        /* shifting Ci and Di */
        for (j = 0; j < iteration_shift[i]; j++) {
            
            C = (0x0fffffff & (C << 1)) | (0x00000001 & (C >> 27));
            D = (0x0fffffff & (D << 1)) | (0x00000001 & (D >> 27));
            
        }
        
        permuted_choice_2 = 0;
        permuted_choice_2 = (((uint64_t) C) << 28) | (uint64_t) D ;
        
        sub_key[i] = 0;
        
        for (j = 0; j < 48; j++) {
            
            sub_key[i] <<= 1;
            sub_key[i] |= (permuted_choice_2 >> (56-PC2[j])) & LB64_MASK;
            
        }
        
    }
    
    for (i = 0; i < 16; i++) {
        
        /* f(R,k) function */
        s_input = 0;
        
        for (j = 0; j< 48; j++) {
            
            s_input <<= 1;
            s_input |= (uint64_t) ((R >> (32-E[j])) & LB32_MASK);
            
        }
        
        /* 
         * Encryption/Decryption 
         * XORing expanded Ri with Ki
         */
        if (mode == 'd') {
            /* decryption */
            s_input = s_input ^ sub_key[15-i];
            
        } else {
            /* encryption */
            s_input = s_input ^ sub_key[i];
            
        }
        
        /* S-Box Tables */
        for (j = 0; j < 8; j++) {
            /* 00 00 RCCC CR00 00 00 00 00 00 s_input
               00 00 1000 0100 00 00 00 00 00 row mask
               00 00 0111 1000 00 00 00 00 00 column mask */
            
            row = (char) ((s_input & (0x0000840000000000 >> 6*j)) >> (42-6*j));
            row = (row >> 4) | (row & 0x01);
            
            column = (char) ((s_input & (0x0000780000000000 >> 6*j)) >> (43-6*j));
            
            s_output <<= 4;
            s_output |= (uint32_t) (S[j][16*row + column] & 0x0f);
            
        }
        
        f_function_res = 0;
        
        for (j = 0; j < 32; j++) {
            
            f_function_res <<= 1;
            f_function_res |= (s_output >> (32 - P[j])) & LB32_MASK;
            
        }
        
        temp = R;
        R = L ^ f_function_res;
        L = temp;
        
    }
    
    pre_output = (((uint64_t) R) << 32) | (uint64_t) L;
        
    /* inverse initial permutation */
    for (i = 0; i < 64; i++) {
        
        inv_init_perm_res <<= 1;
        inv_init_perm_res |= (pre_output >> (64-PI[i])) & LB64_MASK;
        
    }
    
    return inv_init_perm_res;
    
}


static uint64_t _reverse_uint64( uint64_t a ) {
    uint64_t b = 0;
    b |= (a >> 56) & 0x00000000000000FF;
    b |= (a << 56) & 0xFF00000000000000;
    b |= (a >> 40) & 0x000000000000FF00;
    b |= (a << 40) & 0x00FF000000000000;
    b |= (a >> 24) & 0x0000000000FF0000;
    b |= (a << 24) & 0x0000FF0000000000;
    b |= (a >> 8 ) & 0x00000000FF000000;
    b |= (a << 8 ) & 0x000000FF00000000;
    return b;
}

static int _is_big_endian() {
    uint16_t a = 0xFF00;
    uint8_t *c = (uint8_t *)&a;
    return *c == 0xFF;
}

static void _des(char in[8], char k[8], char out[8], char mode) {
    uint64_t input;
    uint64_t key;
    uint64_t output;
    
    input = *(uint64_t *)(in);
    key = *(uint64_t *)(k);
    if(!_is_big_endian()) {
        input = _reverse_uint64(input); 
        key = _reverse_uint64(key);
    }
    output = _des_uint64(input, key, mode);
    if(!_is_big_endian()) {
        output = _reverse_uint64(output);
    }
    *(uint64_t *)(out) = output;
}

/*
 * des_ecb_pkcs5
 * Need to free '*out' after call this.
 */
static void des_ecb_pkcs5(char * in, size_t size_in, char key[8], char ** out, size_t *size_out, char mode) {
    size_t r;
    size_t len;
    size_t i;
    char data[8];
    char * data_out;

    *out = 0;
    *size_out = 0;

    if(mode == 'e') {
        r = size_in % 8;
        len = size_in + 8 - r;
        data_out = malloc(len);
        for(i = 0; i < len; i += 8) {
            if(i < len-8) {
                _des(in + i, key, data_out + i, mode);
            }
            else {
                memcpy(data, in + i, r);
                memset(data + r, 8 - r, 8 - r);
                _des(data, key, data_out + i, mode);
            }
        }
        *out = data_out;
        *size_out = len;
    }
    else if(mode == 'd') {
        len = size_in;
        data_out = malloc(len);
        for(i = 0; i < len; i+=8) {
            _des(in + i, key, data_out + i, mode); 
        }
        *out = data_out;
        *size_out = len - data_out[len-1];
    }
}

/*
int test_des() {
    char *input = "helloworldwhatab";
    char *key = "chivox.com";
    char *data2 = 0;
    size_t outsize = 0;
    char * data3 = 0;
    size_t data3size = 0;
    size_t i;

    printf("input = %s\n", input);
    des_ecb_pkcs5(input, strlen(input), key, &data2, &outsize, 'e');

    printf("outsize = %ld\n", outsize);
    for(i = 0; i < outsize; i++) {
        printf("%02X ", (uint8_t) (data2[i]));
    }
    printf("\n");

    des_ecb_pkcs5(data2, outsize, key, &data3, &data3size, 'd'); 
    printf("data3size = %ld\n", data3size);
    for(i = 0; i < data3size; i++) {
        printf("%02X ", (uint8_t) (data3[i]));
    }

    data3[data3size] = 0;
    printf("data3 = %s\n", data3);
    printf("\n");

    free(data2);
    free(data3);
    return 0;
}
*/
