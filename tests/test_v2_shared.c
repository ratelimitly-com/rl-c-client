#include "r_crypto.h"
#include "r_protocol.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
static void unhex(const char *s, unsigned char *out) { for(size_t i=0;s[2*i];i++){ unsigned x; assert(sscanf(s+2*i,"%2x",&x)==1);out[i]=(unsigned char)x;}}
int main(void) { r_auth_key_info_wide_t info; unsigned char id[16],expected[1200],body[1200],pdu[1200]; size_t n,pn;
assert(r_client_parse_auth_key_wide("rl-cookie1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtrusg08tcva",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(1000));
assert(r_client_parse_auth_key_wide("rl-cookie1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtrussyj8v8n",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(5000));
assert(r_client_parse_auth_key_wide("rl-cookie1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtrusc6xrqew",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(10000));
assert(r_client_parse_auth_key_wide("rl-cookie1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtru3q7wqwex",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(15000));
assert(r_client_parse_auth_key_wide("rl-cookie1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtru3gq6yz8m",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(30000));
assert(r_client_parse_auth_key_wide("rl-cookie1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtru3st0gkv4",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(60000));
assert(r_client_parse_auth_key_wide("rl-cookie1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtru3c4mv6jg",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(120000));
assert(r_client_parse_auth_key_wide("rl-cookie1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtrujq0q3fyv",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(180000));
assert(r_client_parse_auth_key_wide("rl-cookie1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtrujg354963",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(300000));
assert(r_client_parse_auth_key_wide("rl-cookie1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtrujs6pe33l",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(600000));
assert(r_client_parse_auth_key_wide("rl-cookie1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtrujcy4aa0z",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(900000));
assert(r_client_parse_auth_key_wide("rl-cookie1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtrunqqa7n02",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(1800000));
assert(r_client_parse_auth_key_wide("rl-cookie1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtrung7f6l3h",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(2700000));
assert(r_client_parse_auth_key_wide("rl-cookie1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtruns4ukt6e",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(3600000));
assert(r_client_parse_auth_key_wide("rl-cookie1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtrunctgj8yy",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(7200000));
assert(r_client_parse_auth_key_wide("rl-cookie1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtru5qyu68hc",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(10800000));
assert(r_client_parse_auth_key_wide("rl-cookie1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtru5g6g7tf9",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(18000000));
assert(r_client_parse_auth_key_wide("rl-cookie1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtru5s3ajlzt",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(21600000));
assert(r_client_parse_auth_key_wide("rl-cookie1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtru5c0fknuk",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(28800000));
assert(r_client_parse_auth_key_wide("rl-cookie1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtru4qtp4au7",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(43200000));
assert(r_client_parse_auth_key_wide("rl-cookie1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtru4g4433zr",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(86400000));
assert(r_client_parse_auth_key_wide("rl-cookie1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtru4s7qa9fd",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(172800000));
assert(r_client_parse_auth_key_wide("rl-cookie1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtru4cq5efhs",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(432000000));
assert(r_client_parse_auth_key_wide("rl-cookie1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtrukq60y6p5",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(604800000));
assert(r_client_parse_auth_key_wide("rl-cookie1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtrukgymqklf",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(1209600000));
assert(r_client_parse_auth_key_wide("rl-cookie1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtruks0wvz58",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(2419200000));
assert(r_client_parse_auth_key_wide("rl-cookie1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtrukc36gw26",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(2592000000));
assert(r_client_parse_auth_key_wide("rl-cookie1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtruhq4jtq2j",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(7776000000));
assert(r_client_parse_auth_key_wide("rl-cookie1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtruhgtx0v50",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(15724800000));
assert(r_client_parse_auth_key_wide("rl-cookie1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtruhsqnrclp",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(31536000000));
assert(r_client_parse_auth_key_wide("rl-cookie1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtruhc7885pu",&info)==0); assert(info.format_version==2); assert(info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==0);
assert(r_client_parse_auth_key_wide("rl-aes1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtrusgl8u0g8",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(1000));
assert(r_client_parse_auth_key_wide("rl-aes1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtruss5jsmrf",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(5000));
assert(r_client_parse_auth_key_wide("rl-aes1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtrusc2x5ha5",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(10000));
assert(r_client_parse_auth_key_wide("rl-aes1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtru3qwwheau",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(15000));
assert(r_client_parse_auth_key_wide("rl-aes1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtru3gs6n4rp",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(30000));
assert(r_client_parse_auth_key_wide("rl-aes1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtru3sm0lpg0",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(60000));
assert(r_client_parse_auth_key_wide("rl-aes1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtru3c9mmdkj",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(120000));
assert(r_client_parse_auth_key_wide("rl-aes1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtrujqlqx7qk",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(180000));
assert(r_client_parse_auth_key_wide("rl-aes1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtrujgp5zj7t",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(300000));
assert(r_client_parse_auth_key_wide("rl-aes1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtrujs2pwx49",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(600000));
assert(r_client_parse_auth_key_wide("rl-aes1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtrujc5422tc",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(900000));
assert(r_client_parse_auth_key_wide("rl-aes1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtrunqsafyts",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(1800000));
assert(r_client_parse_auth_key_wide("rl-aes1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtrungwfdg4d",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(2700000));
assert(r_client_parse_auth_key_wide("rl-aes1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtruns9upu7r",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(3600000));
assert(r_client_parse_auth_key_wide("rl-aes1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtruncmg9sq7",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(7200000));
assert(r_client_parse_auth_key_wide("rl-aes1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtru5q5udsnz",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(10800000));
assert(r_client_parse_auth_key_wide("rl-aes1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtru5g2gfudl",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(18000000));
assert(r_client_parse_auth_key_wide("rl-aes1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtru5spa9gx3",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(21600000));
assert(r_client_parse_auth_key_wide("rl-aes1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtru5clfpycv",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(28800000));
assert(r_client_parse_auth_key_wide("rl-aes1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtru4qmpz2cy",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(43200000));
assert(r_client_parse_auth_key_wide("rl-aes1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtru4g94xxxe",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(86400000));
assert(r_client_parse_auth_key_wide("rl-aes1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtru4swq2jdh",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(172800000));
assert(r_client_parse_auth_key_wide("rl-aes1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtru4cs5w7n2",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(432000000));
assert(r_client_parse_auth_key_wide("rl-aes1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtrukq20nd9w",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(604800000));
assert(r_client_parse_auth_key_wide("rl-aes1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtrukg5mhpmn",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(1209600000));
assert(r_client_parse_auth_key_wide("rl-aes1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtrukslwm4sa",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(2419200000));
assert(r_client_parse_auth_key_wide("rl-aes1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtrukcp6lewq",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(2592000000));
assert(r_client_parse_auth_key_wide("rl-aes1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtruhq9juhwg",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(7776000000));
assert(r_client_parse_auth_key_wide("rl-aes1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtruhgmxcms4",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(15724800000));
assert(r_client_parse_auth_key_wide("rl-aes1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtruhssn50mm",&info)==0); assert(info.format_version==2); assert(!info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==UINT64_C(31536000000));
assert(r_client_parse_auth_key_wide("rl-aes1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtruhcw8sr9x",&info)==0); assert(info.format_version==2); assert(info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==0);
assert(r_client_parse_auth_key_wide("rl-aes1qgpqqqqqqqqqqqqzqgpqyqszqgpqyqszqgpqyqszqgpqyqszqgpqyqszqgpqyqszqghjmuhcchgqf0",&info)==0); assert(info.format_version==2); assert(info.rate_window_cap.unlimited && info.rate_window_cap.milliseconds==0);
assert(r_client_parse_auth_key_wide("rl-none1qgyqwps9qspsyq2sk8eqq9x8s2q",&info)!=0);
assert(r_client_parse_auth_key_wide("rl-none1qvyqwps9qspsyq2sk8erq7u3ws4",&info)!=0);
assert(r_client_parse_auth_key_wide("rl-none1qgyqwps9qspsyq2ek8erq3lmvjl",&info)!=0);
assert(r_client_parse_auth_key_wide("rl-none1qgyqwps9qspsyqfsk0erqrmw3u8",&info)!=0);
assert(r_client_parse_auth_key_wide("rl-none1qgyqwps9qspsyq2skyprqh9za64",&info)!=0);
assert(r_client_parse_auth_key_wide("rl-none1qgyqwps9qspsyq2sk99rvlllrly",&info)!=0);
assert(r_client_parse_auth_key_wide("rl-none1qgyqwps9qspsyq2sk8eq3d3mar",&info)!=0);
assert(r_client_parse_auth_key_wide("rl-none1qgyqwps9qspsyq2sk8erqqqmeq20l",&info)!=0);
assert(r_client_parse_auth_key_wide("rl-none1qgyqwps9qspsyq2sk8erq5gkhhq",&info)!=0);
assert(r_client_parse_auth_key_wide("rl-cookie1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtrusq3n05jq",&info)!=0);
assert(r_client_parse_auth_key_wide("rl-cookie1qvyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtru3sty6sps",&info)!=0);
assert(r_client_parse_auth_key_wide("rl-cookie1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ravmru3sgxrplk",&info)!=0);
assert(r_client_parse_auth_key_wide("rl-cookie1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ruct8u3s9k5m8u",&info)!=0);
assert(r_client_parse_auth_key_wide("rl-cookie1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtzq3stj50gt",&info)!=0);
assert(r_client_parse_auth_key_wide("rl-cookie1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtzj3k6scugj",&info)!=0);
assert(r_client_parse_auth_key_wide("rl-cookie1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtrus3eynem",&info)!=0);
assert(r_client_parse_auth_key_wide("rl-cookie1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtru3sqq9tdqg2",&info)!=0);
assert(r_client_parse_auth_key_wide("rl-cookie1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtru3st0gkvq",&info)!=0);
assert(r_client_parse_auth_key_wide("rl-aes1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtrusqpncrk6",&info)!=0);
assert(r_client_parse_auth_key_wide("rl-aes1qvyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtru3smyd892",&info)!=0);
assert(r_client_parse_auth_key_wide("rl-aes1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ravmru3scx5kmv",&info)!=0);
assert(r_client_parse_auth_key_wide("rl-aes1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ruct8u3s4krvrx",&info)!=0);
assert(r_client_parse_auth_key_wide("rl-aes1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtzq3smjrcv3",&info)!=0);
assert(r_client_parse_auth_key_wide("rl-aes1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtzj3k2s0tvg",&info)!=0);
assert(r_client_parse_auth_key_wide("rl-aes1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtrus4xwxgs",&info)!=0);
assert(r_client_parse_auth_key_wide("rl-aes1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtru3sqq5f0yu3",&info)!=0);
assert(r_client_parse_auth_key_wide("rl-aes1qgyqwps9qspsyqgqqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5z5tpwxqergd3c8g7ragtru3sm0lpgq",&info)!=0);
assert(r_client_derive_bucket_id_wide("\x63\x68\x65\x63\x6b\x6f\x75\x74",8,UINT64_C(1000),UINT32_C(100),id)==0);unhex("f5cf3ad8b8406854b596ba3614f16eff",expected);assert(memcmp(id,expected,16)==0);
assert(r_client_derive_bucket_id_wide("\x63\x61\x66\xc3\xa9",5,UINT64_C(60000),UINT32_C(25),id)==0);unhex("732b120606917c683e37433fa00a60dc",expected);assert(memcmp(id,expected,16)==0);
assert(r_client_derive_bucket_id_wide("\x62\x69\x6e\x61\x72\x79\x00\x62\x75\x63\x6b\x65\x74",13,UINT64_C(4294967295),UINT32_C(4294967295),id)==0);unhex("f57a48f69aa8c7a16fc73499ae8d07fa",expected);assert(memcmp(id,expected,16)==0);
assert(r_client_derive_bucket_id_wide("\x61",1,UINT64_C(1),UINT32_C(1),id)==0);unhex("bfc1f923dc71627f301ab603438dab52",expected);assert(memcmp(id,expected,16)==0);
assert(r_client_derive_bucket_id_wide("\x63\x68\x65\x63\x6b\x6f\x75\x74",8,UINT64_C(4294967296),UINT32_C(100),id)==0);unhex("e88e90a49fa4fe0009a88c6df13c6db6",expected);assert(memcmp(id,expected,16)==0);
assert(r_client_derive_bucket_id_wide("\x63\x68\x65\x63\x6b\x6f\x75\x74",8,UINT64_C(7776000000),UINT32_C(100),id)==0);unhex("f982a492b559894e4983dba3fd147776",expected);assert(memcmp(id,expected,16)==0);
assert(r_client_derive_bucket_id_wide("\x63\x68\x65\x63\x6b\x6f\x75\x74",8,UINT64_C(15724800000),UINT32_C(100),id)==0);unhex("b7efa2e9f80bad68cc5d1bcf2f28498b",expected);assert(memcmp(id,expected,16)==0);
assert(r_client_derive_bucket_id_wide("\x63\x68\x65\x63\x6b\x6f\x75\x74",8,UINT64_C(31536000000),UINT32_C(100),id)==0);unhex("ed55bbd9f85d6541d52f8d9c1c67ed63",expected);assert(memcmp(id,expected,16)==0);
{r_resource_request_t rs[1]={0};
unhex("93afd4811d83da29c48a7a83161e3094",rs[0].bucket_id);rs[0].window_size_ms=UINT64_C(1);rs[0].rate_limit=100;rs[0].tokens_requested=1;
assert(r_build_rate_request_body(rs,1,NULL,0,NULL,0,body,sizeof(body),&n)==0);assert(r_build_rate_request_pdu(300,body,n,pdu,sizeof(pdu),&pn)==0);
unhex("525428002c0100000000010093afd4811d83da29c48a7a83161e3094010000006400000001000000",expected);assert(pn==40);assert(memcmp(pdu,expected,pn)==0);
unhex("52522800000000000000010093afd4811d83da29c48a7a83161e3094010000000100000000000000",pdu);r_resource_result_t rr[16];r_guard_result_t gg[16];size_t gc,rc;bool ok;assert(r_parse_rate_response_pdu(pdu,pn,gg,16,&gc,rr,16,&rc,&ok)==0);assert(ok && rc==1);
}
{r_resource_request_wide_t rs[1]={0};
unhex("93afd4811d83da29c48a7a83161e3094",rs[0].bucket_id);rs[0].window_size_ms=UINT64_C(1);rs[0].rate_limit=100;rs[0].tokens_requested=1;
assert(r_build_rate_request_body_wide(rs,1,NULL,0,NULL,0,body,sizeof(body),&n)==0);assert(r_build_rate_request_pdu(300,body,n,pdu,sizeof(pdu),&pn)==0);
pdu[0]=0x51;pdu[1]=0x57;
unhex("51572c002c0100000000010093afd4811d83da29c48a7a83161e309401000000000000006400000001000000",expected);assert(pn==44);assert(memcmp(pdu,expected,pn)==0);
unhex("52572c00000000000000010093afd4811d83da29c48a7a83161e309401000000000000000100000000000000",pdu);r_resource_result_t rr[16];r_guard_result_t gg[16];size_t gc,rc;bool ok;assert(r_parse_rate_response_pdu(pdu,pn,gg,16,&gc,rr,16,&rc,&ok)==0);assert(ok && rc==1);
}
{r_resource_request_t rs[1]={0};
unhex("cc7f223ea2635df682138d3302df8fe7",rs[0].bucket_id);rs[0].window_size_ms=UINT64_C(60000);rs[0].rate_limit=100;rs[0].tokens_requested=1;
assert(r_build_rate_request_body(rs,1,NULL,0,NULL,0,body,sizeof(body),&n)==0);assert(r_build_rate_request_pdu(300,body,n,pdu,sizeof(pdu),&pn)==0);
unhex("525428002c01000000000100cc7f223ea2635df682138d3302df8fe760ea00006400000001000000",expected);assert(pn==40);assert(memcmp(pdu,expected,pn)==0);
unhex("525228000000000000000100cc7f223ea2635df682138d3302df8fe760ea00000100000000000000",pdu);r_resource_result_t rr[16];r_guard_result_t gg[16];size_t gc,rc;bool ok;assert(r_parse_rate_response_pdu(pdu,pn,gg,16,&gc,rr,16,&rc,&ok)==0);assert(ok && rc==1);
}
{r_resource_request_wide_t rs[1]={0};
unhex("cc7f223ea2635df682138d3302df8fe7",rs[0].bucket_id);rs[0].window_size_ms=UINT64_C(60000);rs[0].rate_limit=100;rs[0].tokens_requested=1;
assert(r_build_rate_request_body_wide(rs,1,NULL,0,NULL,0,body,sizeof(body),&n)==0);assert(r_build_rate_request_pdu(300,body,n,pdu,sizeof(pdu),&pn)==0);
pdu[0]=0x51;pdu[1]=0x57;
unhex("51572c002c01000000000100cc7f223ea2635df682138d3302df8fe760ea0000000000006400000001000000",expected);assert(pn==44);assert(memcmp(pdu,expected,pn)==0);
unhex("52572c000000000000000100cc7f223ea2635df682138d3302df8fe760ea0000000000000100000000000000",pdu);r_resource_result_t rr[16];r_guard_result_t gg[16];size_t gc,rc;bool ok;assert(r_parse_rate_response_pdu(pdu,pn,gg,16,&gc,rr,16,&rc,&ok)==0);assert(ok && rc==1);
}
{r_resource_request_t rs[1]={0};
unhex("99510d8b29a435fb81c010a9c08b550b",rs[0].bucket_id);rs[0].window_size_ms=UINT64_C(4294967295);rs[0].rate_limit=100;rs[0].tokens_requested=1;
assert(r_build_rate_request_body(rs,1,NULL,0,NULL,0,body,sizeof(body),&n)==0);assert(r_build_rate_request_pdu(300,body,n,pdu,sizeof(pdu),&pn)==0);
unhex("525428002c0100000000010099510d8b29a435fb81c010a9c08b550bffffffff6400000001000000",expected);assert(pn==40);assert(memcmp(pdu,expected,pn)==0);
unhex("52522800000000000000010099510d8b29a435fb81c010a9c08b550bffffffff0100000000000000",pdu);r_resource_result_t rr[16];r_guard_result_t gg[16];size_t gc,rc;bool ok;assert(r_parse_rate_response_pdu(pdu,pn,gg,16,&gc,rr,16,&rc,&ok)==0);assert(ok && rc==1);
}
{r_resource_request_wide_t rs[1]={0};
unhex("99510d8b29a435fb81c010a9c08b550b",rs[0].bucket_id);rs[0].window_size_ms=UINT64_C(4294967295);rs[0].rate_limit=100;rs[0].tokens_requested=1;
assert(r_build_rate_request_body_wide(rs,1,NULL,0,NULL,0,body,sizeof(body),&n)==0);assert(r_build_rate_request_pdu(300,body,n,pdu,sizeof(pdu),&pn)==0);
pdu[0]=0x51;pdu[1]=0x57;
unhex("51572c002c0100000000010099510d8b29a435fb81c010a9c08b550bffffffff000000006400000001000000",expected);assert(pn==44);assert(memcmp(pdu,expected,pn)==0);
unhex("52572c00000000000000010099510d8b29a435fb81c010a9c08b550bffffffff000000000100000000000000",pdu);r_resource_result_t rr[16];r_guard_result_t gg[16];size_t gc,rc;bool ok;assert(r_parse_rate_response_pdu(pdu,pn,gg,16,&gc,rr,16,&rc,&ok)==0);assert(ok && rc==1);
}
{r_resource_request_wide_t rs[1]={0};
unhex("e88e90a49fa4fe0009a88c6df13c6db6",rs[0].bucket_id);rs[0].window_size_ms=UINT64_C(4294967296);rs[0].rate_limit=100;rs[0].tokens_requested=1;
assert(r_build_rate_request_body_wide(rs,1,NULL,0,NULL,0,body,sizeof(body),&n)==0);assert(r_build_rate_request_pdu(300,body,n,pdu,sizeof(pdu),&pn)==0);
pdu[0]=0x51;pdu[1]=0x57;
unhex("51572c002c01000000000100e88e90a49fa4fe0009a88c6df13c6db600000000010000006400000001000000",expected);assert(pn==44);assert(memcmp(pdu,expected,pn)==0);
unhex("52572c000000000000000100e88e90a49fa4fe0009a88c6df13c6db600000000010000000100000000000000",pdu);r_resource_result_t rr[16];r_guard_result_t gg[16];size_t gc,rc;bool ok;assert(r_parse_rate_response_pdu(pdu,pn,gg,16,&gc,rr,16,&rc,&ok)==0);assert(ok && rc==1);
}
{r_resource_request_wide_t rs[1]={0};
unhex("f982a492b559894e4983dba3fd147776",rs[0].bucket_id);rs[0].window_size_ms=UINT64_C(7776000000);rs[0].rate_limit=100;rs[0].tokens_requested=1;
assert(r_build_rate_request_body_wide(rs,1,NULL,0,NULL,0,body,sizeof(body),&n)==0);assert(r_build_rate_request_pdu(300,body,n,pdu,sizeof(pdu),&pn)==0);
pdu[0]=0x51;pdu[1]=0x57;
unhex("51572c002c01000000000100f982a492b559894e4983dba3fd14777600587ccf010000006400000001000000",expected);assert(pn==44);assert(memcmp(pdu,expected,pn)==0);
unhex("52572c000000000000000100f982a492b559894e4983dba3fd14777600587ccf010000000100000000000000",pdu);r_resource_result_t rr[16];r_guard_result_t gg[16];size_t gc,rc;bool ok;assert(r_parse_rate_response_pdu(pdu,pn,gg,16,&gc,rr,16,&rc,&ok)==0);assert(ok && rc==1);
}
{r_resource_request_wide_t rs[1]={0};
unhex("b7efa2e9f80bad68cc5d1bcf2f28498b",rs[0].bucket_id);rs[0].window_size_ms=UINT64_C(15724800000);rs[0].rate_limit=100;rs[0].tokens_requested=1;
assert(r_build_rate_request_body_wide(rs,1,NULL,0,NULL,0,body,sizeof(body),&n)==0);assert(r_build_rate_request_pdu(300,body,n,pdu,sizeof(pdu),&pn)==0);
pdu[0]=0x51;pdu[1]=0x57;
unhex("51572c002c01000000000100b7efa2e9f80bad68cc5d1bcf2f28498b006845a9030000006400000001000000",expected);assert(pn==44);assert(memcmp(pdu,expected,pn)==0);
unhex("52572c000000000000000100b7efa2e9f80bad68cc5d1bcf2f28498b006845a9030000000100000000000000",pdu);r_resource_result_t rr[16];r_guard_result_t gg[16];size_t gc,rc;bool ok;assert(r_parse_rate_response_pdu(pdu,pn,gg,16,&gc,rr,16,&rc,&ok)==0);assert(ok && rc==1);
}
{r_resource_request_wide_t rs[1]={0};
unhex("ed55bbd9f85d6541d52f8d9c1c67ed63",rs[0].bucket_id);rs[0].window_size_ms=UINT64_C(31536000000);rs[0].rate_limit=100;rs[0].tokens_requested=1;
assert(r_build_rate_request_body_wide(rs,1,NULL,0,NULL,0,body,sizeof(body),&n)==0);assert(r_build_rate_request_pdu(300,body,n,pdu,sizeof(pdu),&pn)==0);
pdu[0]=0x51;pdu[1]=0x57;
unhex("51572c002c01000000000100ed55bbd9f85d6541d52f8d9c1c67ed63002cb157070000006400000001000000",expected);assert(pn==44);assert(memcmp(pdu,expected,pn)==0);
unhex("52572c000000000000000100ed55bbd9f85d6541d52f8d9c1c67ed63002cb157070000000100000000000000",pdu);r_resource_result_t rr[16];r_guard_result_t gg[16];size_t gc,rc;bool ok;assert(r_parse_rate_response_pdu(pdu,pn,gg,16,&gc,rr,16,&rc,&ok)==0);assert(ok && rc==1);
}
{r_resource_request_wide_t rs[2]={0};
unhex("cc7f223ea2635df682138d3302df8fe7",rs[0].bucket_id);rs[0].window_size_ms=UINT64_C(60000);rs[0].rate_limit=100;rs[0].tokens_requested=1;
unhex("ed55bbd9f85d6541d52f8d9c1c67ed63",rs[1].bucket_id);rs[1].window_size_ms=UINT64_C(31536000000);rs[1].rate_limit=100;rs[1].tokens_requested=1;
assert(r_build_rate_request_body_wide(rs,2,NULL,0,NULL,0,body,sizeof(body),&n)==0);assert(r_build_rate_request_pdu(300,body,n,pdu,sizeof(pdu),&pn)==0);
pdu[0]=0x51;pdu[1]=0x57;
unhex("51574c002c01000000000200cc7f223ea2635df682138d3302df8fe760ea0000000000006400000001000000ed55bbd9f85d6541d52f8d9c1c67ed63002cb157070000006400000001000000",expected);assert(pn==76);assert(memcmp(pdu,expected,pn)==0);
unhex("52574c000000000000000200cc7f223ea2635df682138d3302df8fe760ea0000000000000100000000000000ed55bbd9f85d6541d52f8d9c1c67ed63002cb157070000000100000000000000",pdu);r_resource_result_t rr[16];r_guard_result_t gg[16];size_t gc,rc;bool ok;assert(r_parse_rate_response_pdu(pdu,pn,gg,16,&gc,rr,16,&rc,&ok)==0);assert(ok && rc==2);
}
puts("shared v2 caps/IDs/wire PASS");return 0;}