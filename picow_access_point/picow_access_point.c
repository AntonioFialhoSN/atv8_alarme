/**
 * Projeto: Servidor HTTP com controle de Alarme via Access Point - Raspberry Pi Pico W
 * Controle do LED vermelho no pino 13 e buzzer no pino 21 como alarme
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/pwm.h"
#include "hardware/irq.h"
#include "hardware/i2c.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "dhcpserver.h"
#include "dnsserver.h"
#include "inc/ssd1306.h"

// =============================================
// Configurações de Hardware
// =============================================

// Configuração dos GPIOs
#define RED_LED_GPIO       13      // Pino do LED vermelho do alarme
#define PWM_GPIO          21      // Pino do buzzer (PWM)
#define I2C_SDA           14      // Pino I2C SDA para o display
#define I2C_SCL           15      // Pino I2C SCL para o display

// Configuração do Buzzer PWM
#define PWM_FREQ_HZ       1000    // Frequência do bipe em Hz (1 kHz)
#define CLOCK_DIV         2.0f    // Divisor ajustado para evitar overflow
#define PWM_WRAP          (uint16_t)(125000000 / (PWM_FREQ_HZ * CLOCK_DIV))  // = 62500
#define BEEP_DURATION_MS  200     // Duração de cada bipe em ms
#define BEEP_INTERVAL_MS  100     // Intervalo entre bipes em ms

// Configuração do Display OLED
#define SSD1306_I2C_ADDR  0x3C    // Endereço I2C do display OLED

// =============================================
// Configurações de Rede
// =============================================
#define WIFI_SSID         "alarmeresidencial"
#define WIFI_PASSWORD     "12345678"
#define PORTA_TCP         80      // Porta HTTP padrão
#define IP_GW             "192.168.4.1"  // Gateway IP
#define IP_MASK           "255.255.255.0"

// =============================================
// Configurações do Servidor HTTP
// =============================================
#define TEMPO_POLLING     5
#define HTTP_GET          "GET"
#define HTTP_RESPONSE_HEADERS "HTTP/1.1 %d OK\nContent-Length: %d\nContent-Type: text/html; charset=utf-8\nConnection: close\n\n"
#define ALARM_CONTROL_BODY "<html><body style=\"text-align:center;margin-top:50px\">" \
"<h1>Alarme</h1>" \
"<p>%s</p>" \
"<a href=\"?alarm=%d\" style=\"background:#4CAF50;color:white;padding:5px 10px;text-decoration:none\">%s</a>" \
"</body></html>"
#define ALARM_PARAM       "alarm=%d"
#define ALARM_CONTROL     "/alarm"
#define HTTP_RESPONSE_REDIRECT "HTTP/1.1 302 Redirect\nLocation: http://%s" ALARM_CONTROL "\n\n"

// =============================================
// Estruturas de Dados
// =============================================

// Área de renderização do display
struct render_area display_area = {
    .start_column = 0,
    .end_column = ssd1306_width - 1,
    .start_page = 0,
    .end_page = ssd1306_n_pages - 1
};

uint8_t ssd1306_buffer[ssd1306_buffer_length]; // Buffer do display

typedef struct TCP_SERVER_T_ {
    struct tcp_pcb *server_pcb;
    bool complete;
    ip_addr_t gw;
    bool alarm_active;           // Flag para controlar o estado do alarme
    absolute_time_t next_toggle_time;  // Próximo momento para alternar o LED/buzzer
    bool beep_active;            // Flag para controlar o estado do bipe
    absolute_time_t beep_end_time; // Quando o bipe atual deve terminar
} TCP_SERVER_T;

typedef struct TCP_CONNECT_STATE_T_ {
    struct tcp_pcb *pcb;
    int sent_len;
    char headers[128];
    char result[256];
    int header_len;
    int result_len;
    ip_addr_t *gw;
    TCP_SERVER_T *server_state;  // Ponteiro para o estado do servidor
} TCP_CONNECT_STATE_T;

// =============================================
// Funções do Display OLED
// =============================================

void display_message(const char *line1, const char *line2) {
    ssd1306_clear_display(ssd1306_buffer);
    
    if (line1) {
        int x1 = (ssd1306_width - strlen(line1) * 6) / 2; // 6px por caractere
        ssd1306_draw_string(ssd1306_buffer, x1, 20, line1); // Y = 20px
    }
    
    if (line2) {
        int x2 = (ssd1306_width - strlen(line2) * 6) / 2;
        ssd1306_draw_string(ssd1306_buffer, x2, 30, line2); // Y = 30px
    }
    
    render_on_display(ssd1306_buffer, &display_area);
}

// =============================================
// Funções de Controle do Alarme
// =============================================

void update_alarm(TCP_SERVER_T *state) {
    if (state->alarm_active) {
        // Alarme ativado - piscar o LED e emitir bipes
        if (absolute_time_diff_us(get_absolute_time(), state->next_toggle_time) <= 0) {
            static bool led_state = false;
            led_state = !led_state;
            gpio_put(RED_LED_GPIO, led_state);
            
            // Ativa/desativa o buzzer
            if (led_state) {
                pwm_set_gpio_level(PWM_GPIO, PWM_WRAP / 2);  // Duty 50%
                state->beep_active = true;
                state->beep_end_time = delayed_by_us(get_absolute_time(), BEEP_DURATION_MS * 1000);
            } else {
                pwm_set_gpio_level(PWM_GPIO, 0);  // Silencia buzzer
                state->beep_active = false;
            }
            
            // Configura o próximo tempo de alternância
            state->next_toggle_time = delayed_by_us(get_absolute_time(), BEEP_INTERVAL_MS * 1000);
        }
        
        // Desativa o buzzer após o tempo configurado
        if (state->beep_active && absolute_time_diff_us(get_absolute_time(), state->beep_end_time) <= 0) {
            pwm_set_gpio_level(PWM_GPIO, 0);
            state->beep_active = false;
        }
        
        // Atualiza display com mensagem de alarme ativo
        display_message("ALARME", "EVACUAR");
    } else {
        // Alarme desativado - LED apagado e buzzer silenciado
        gpio_put(RED_LED_GPIO, 0);
        pwm_set_gpio_level(PWM_GPIO, 0);
        state->beep_active = false;
        // Atualiza display com mensagem de repouso
        display_message("Sistema", "em repouso");
    }
}

// =============================================
// Funções do Servidor TCP/HTTP
// =============================================

static err_t tcp_close_client_connection(TCP_CONNECT_STATE_T *con_state, struct tcp_pcb *client_pcb, err_t close_err) {
    if (client_pcb) {
        tcp_arg(client_pcb, NULL);
        tcp_poll(client_pcb, NULL, 0);
        tcp_sent(client_pcb, NULL);
        tcp_recv(client_pcb, NULL);
        tcp_err(client_pcb, NULL);
        err_t err = tcp_close(client_pcb);
        if (err != ERR_OK) {
            printf("close failed %d, calling abort\n", err);
            tcp_abort(client_pcb);
            close_err = ERR_ABRT;
        }
        if (con_state) {
            free(con_state);
        }
    }
    return close_err;
}

static void tcp_server_close(TCP_SERVER_T *state) {
    if (state->server_pcb) {
        tcp_arg(state->server_pcb, NULL);
        tcp_close(state->server_pcb);
        state->server_pcb = NULL;
    }
}

static err_t tcp_server_sent(void *arg, struct tcp_pcb *pcb, u16_t len) {
    TCP_CONNECT_STATE_T *con_state = (TCP_CONNECT_STATE_T*)arg;
    printf("tcp_server_sent %u\n", len);
    con_state->sent_len += len;
    if (con_state->sent_len >= con_state->header_len + con_state->result_len) {
        printf("all done\n");
        return tcp_close_client_connection(con_state, pcb, ERR_OK);
    }
    return ERR_OK;
}

static int alarm_control_content(const char *request, const char *params, char *result, size_t max_result_len, TCP_SERVER_T *state) {
    int len = 0;
    if (strncmp(request, ALARM_CONTROL, sizeof(ALARM_CONTROL) - 1) == 0) {
        // Verifica se o usuário alterou o estado do alarme
        if (params) {
            int alarm_param;
            if (sscanf(params, ALARM_PARAM, &alarm_param) == 1) {
                state->alarm_active = alarm_param;
                printf("Alarme %s\n", state->alarm_active ? "ativado" : "desativado");
            }
        }
        
        // Gera a página HTML com o estado atual
        if (state->alarm_active) {
            len = snprintf(result, max_result_len, ALARM_CONTROL_BODY, "ATIVADO", 0, "Desligar");
        } else {
            len = snprintf(result, max_result_len, ALARM_CONTROL_BODY, "DESATIVADO", 1, "Ligar");
        }
    }
    return len;
}

err_t tcp_server_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    TCP_CONNECT_STATE_T *con_state = (TCP_CONNECT_STATE_T*)arg;
    
    if (!p) {
        printf("connection closed\n");
        return tcp_close_client_connection(con_state, pcb, ERR_OK);
    }
    
    if (p->tot_len > 0) {
        printf("tcp_server_recv %d err %d\n", p->tot_len, err);
        
        // Copia a requisição para o buffer
        pbuf_copy_partial(p, con_state->headers, 
                        p->tot_len > sizeof(con_state->headers) - 1 ? sizeof(con_state->headers) - 1 : p->tot_len, 0);

        // Processa requisição GET
        if (strncmp(HTTP_GET, con_state->headers, sizeof(HTTP_GET) - 1) == 0) {
            char *request = con_state->headers + sizeof(HTTP_GET); // + espaço
            char *params = strchr(request, '?');
            
            if (params) {
                if (*params) {
                    char *space = strchr(request, ' ');
                    *params++ = 0;
                    if (space) {
                        *space = 0;
                    }
                } else {
                    params = NULL;
                }
            }

            // Gera conteúdo da página
            con_state->result_len = alarm_control_content(request, params, con_state->result, 
                                                        sizeof(con_state->result), con_state->server_state);
            printf("Request: %s?%s\n", request, params);
            printf("Result: %d\n", con_state->result_len);

            // Verifica se houve espaço suficiente no buffer
            if (con_state->result_len > sizeof(con_state->result) - 1) {
                printf("Too much result data %d\n", con_state->result_len);
                return tcp_close_client_connection(con_state, pcb, ERR_CLSD);
            }

            // Gera a página web
            if (con_state->result_len > 0) {
                con_state->header_len = snprintf(con_state->headers, sizeof(con_state->headers), 
                                               HTTP_RESPONSE_HEADERS, 200, con_state->result_len);
                if (con_state->header_len > sizeof(con_state->headers) - 1) {
                    printf("Too much header data %d\n", con_state->header_len);
                    return tcp_close_client_connection(con_state, pcb, ERR_CLSD);
                }
            } else {
                // Redireciona para a página de controle
                con_state->header_len = snprintf(con_state->headers, sizeof(con_state->headers), 
                                               HTTP_RESPONSE_REDIRECT, ipaddr_ntoa(con_state->gw));
                printf("Sending redirect %s", con_state->headers);
            }

            // Envia os headers para o cliente
            con_state->sent_len = 0;
            err_t err = tcp_write(pcb, con_state->headers, con_state->header_len, 0);
            if (err != ERR_OK) {
                printf("failed to write header data %d\n", err);
                return tcp_close_client_connection(con_state, pcb, err);
            }

            // Envia o corpo da página para o cliente
            if (con_state->result_len) {
                err = tcp_write(pcb, con_state->result, con_state->result_len, 0);
                if (err != ERR_OK) {
                    printf("failed to write result data %d\n", err);
                    return tcp_close_client_connection(con_state, pcb, err);
                }
            }
        }
        tcp_recved(pcb, p->tot_len);
    }
    pbuf_free(p);
    return ERR_OK;
}

static err_t tcp_server_poll(void *arg, struct tcp_pcb *pcb) {
    TCP_CONNECT_STATE_T *con_state = (TCP_CONNECT_STATE_T*)arg;
    printf("tcp_server_poll_fn\n");
    return tcp_close_client_connection(con_state, pcb, ERR_OK);
}

static void tcp_server_err(void *arg, err_t err) {
    TCP_CONNECT_STATE_T *con_state = (TCP_CONNECT_STATE_T*)arg;
    if (err != ERR_ABRT) {
        printf("tcp_client_err_fn %d\n", err);
        tcp_close_client_connection(con_state, con_state->pcb, err);
    }
}

static err_t tcp_server_accept(void *arg, struct tcp_pcb *client_pcb, err_t err) {
    TCP_SERVER_T *state = (TCP_SERVER_T*)arg;
    if (err != ERR_OK || client_pcb == NULL) {
        printf("failure in accept\n");
        return ERR_VAL;
    }
    printf("client connected\n");

    // Create the state for the connection
    TCP_CONNECT_STATE_T *con_state = calloc(1, sizeof(TCP_CONNECT_STATE_T));
    if (!con_state) {
        printf("failed to allocate connect state\n");
        return ERR_MEM;
    }
    con_state->pcb = client_pcb;
    con_state->gw = &state->gw;
    con_state->server_state = state;

    // setup connection to client
    tcp_arg(client_pcb, con_state);
    tcp_sent(client_pcb, tcp_server_sent);
    tcp_recv(client_pcb, tcp_server_recv);
    tcp_poll(client_pcb, tcp_server_poll, TEMPO_POLLING * 2);
    tcp_err(client_pcb, tcp_server_err);

    return ERR_OK;
}

static bool tcp_server_open(void *arg) {
    TCP_SERVER_T *state = (TCP_SERVER_T*)arg;
    printf("starting server on port %d\n", PORTA_TCP);

    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (!pcb) {
        printf("failed to create pcb\n");
        return false;
    }

    err_t err = tcp_bind(pcb, IP_ANY_TYPE, PORTA_TCP);
    if (err) {
        printf("failed to bind to port %d\n", PORTA_TCP);
        return false;
    }

    state->server_pcb = tcp_listen_with_backlog(pcb, 1);
    if (!state->server_pcb) {
        printf("failed to listen\n");
        if (pcb) {
            tcp_close(pcb);
        }
        return false;
    }

    tcp_arg(state->server_pcb, state);
    tcp_accept(state->server_pcb, tcp_server_accept);

    printf("Access Point criado: '%s'\n", WIFI_SSID);
    printf("Conecte-se e acesse: http://%s\n", IP_GW);
    return true;
}

void key_pressed_func(void *param) {
    assert(param);
    TCP_SERVER_T *state = (TCP_SERVER_T*)param;
    int key = getchar_timeout_us(0);
    if (key == 'd' || key == 'D') {
        cyw43_arch_lwip_begin();
        cyw43_arch_disable_ap_mode();
        cyw43_arch_lwip_end();
        state->complete = true;
    }
}

// =============================================
// Função Principal
// =============================================

int main() {
    stdio_init_all();

    TCP_SERVER_T *state = calloc(1, sizeof(TCP_SERVER_T));
    if (!state) {
        printf("failed to allocate state\n");
        return 1;
    }

    if (cyw43_arch_init()) {
        printf("failed to initialise\n");
        return 1;
    }

    // Configuração do hardware
    gpio_init(RED_LED_GPIO);
    gpio_set_dir(RED_LED_GPIO, GPIO_OUT);
    gpio_put(RED_LED_GPIO, 0);

    // Configuração do Buzzer PWM
    gpio_set_function(PWM_GPIO, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num(PWM_GPIO);
    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_clkdiv(&cfg, CLOCK_DIV);
    pwm_config_set_wrap(&cfg, PWM_WRAP);
    pwm_init(slice, &cfg, true);
    pwm_set_gpio_level(PWM_GPIO, 0);

    // Inicialização do display OLED
    i2c_init(i2c1, 400 * 1000);
    gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA);
    gpio_pull_up(I2C_SCL);
    
    // Inicialização do display
    calculate_render_area_buffer_length(&display_area);
    ssd1306_init();
    display_message("Iniciando", "sistema...");

    // Inicializa estado do alarme
    state->alarm_active = false;
    state->beep_active = false;
    state->next_toggle_time = get_absolute_time();

    // Configura callback para tecla pressionada
    stdio_set_chars_available_callback(key_pressed_func, state);

    // Configuração do Access Point
    cyw43_arch_enable_ap_mode(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK);

    // Configuração de rede
    ip4_addr_t mask;
    IP4_ADDR(&state->gw, 192, 168, 4, 1);
    IP4_ADDR(&mask, 255, 255, 255, 0);

    // Inicia servidor DHCP
    dhcp_server_t dhcp_server;
    dhcp_server_init(&dhcp_server, &state->gw, &mask);

    // Inicia servidor DNS
    dns_server_t dns_server;
    dns_server_init(&dns_server, &state->gw);

    if (!tcp_server_open(state)) {
        printf("failed to open server\n");
        return 1;
    }

    state->complete = false;
    while(!state->complete) {
        // Atualiza o estado do alarme (LED e buzzer)
        update_alarm(state);
        
#if PICO_CYW43_ARCH_POLL
        cyw43_arch_poll();
        cyw43_arch_wait_for_work_until(make_timeout_time_ms(10));
#else
        sleep_ms(10);
#endif
    }

    // Limpeza final
    tcp_server_close(state);
    dns_server_deinit(&dns_server);
    dhcp_server_deinit(&dhcp_server);
    
    // Desliga buzzer e LED antes de encerrar
    gpio_put(RED_LED_GPIO, 0);
    pwm_set_gpio_level(PWM_GPIO, 0);

    cyw43_arch_deinit();
    
    printf("Sistema de alarme desligado\n");
    return 0;
}