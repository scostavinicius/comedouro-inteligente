
#include <SPI.h>
#include <MFRC522.h>
#include <ESP32Servo.h>
#include <Preferences.h>
#include "HX711.h"
#include "esp_sleep.h"
#include "driver/gpio.h"

// Dashboard (Adafruit IO)
#include <WiFi.h>
#include <time.h>
#include "Adafruit_MQTT.h"
#include "Adafruit_MQTT_Client.h"

// ==========================================
// WIFI E ADAFRUIT IO
// ==========================================

// No Wokwi a rede e sempre "Wokwi-GUEST", sem senha.
// No projeto real, troque pela rede de casa.
#define WIFI_SSID     "Wokwi-GUEST"
#define WIFI_SENHA    ""

// AIO_USUARIO e AIO_CHAVE ficam em secrets.h, que NAO vai
// para o GitHub. Copie secrets.example.h para secrets.h
// e preencha com os dados do Adafruit IO.
#include "secrets.h"

#define AIO_SERVIDOR  "io.adafruit.com"
#define AIO_PORTA     1883

// Fuso horario de Brasilia (UTC-3, sem horario de verao)
const long FUSO_HORARIO = -3 * 3600;

// ==========================================
// PINOS
// ==========================================

// RFID
#define RFID_SS       5
#define RFID_RST      21

// Servo
#define SERVO_PIN     13

// Botao
#define BUTTON_PIN    25

// LEDs
#define LED_RED       26
#define LED_GREEN     27
#define LED_SLEEP     14

// Sensor PIR
#define PIR_PIN       33

// Balanca HX711
#define HX_DT         16
#define HX_SCK        17

// ==========================================
// CONFIGURACOES
// ==========================================

#define MAX_GATOS 10

const unsigned long TEMPO_SEM_MOVIMENTO = 60000;
const unsigned long DEBOUNCE = 200;

const unsigned long TEMPO_MAXIMO_DOSAGEM = 20000;

const int SERVO_FECHADO = 0;
const int SERVO_ABERTO = 90;

// Fator de calibracao utilizado no teste Wokwi
const float FATOR_CALIBRACAO = 0.42;

// ==========================================
// OBJETOS
// ==========================================

MFRC522 leitor(RFID_SS, RFID_RST);

Servo servo;

Preferences preferencias;

HX711 balanca;

WiFiClient clienteWiFi;

Adafruit_MQTT_Client mqtt(
  &clienteWiFi,
  AIO_SERVIDOR,
  AIO_PORTA,
  AIO_USUARIO,
  AIO_CHAVE
);

// Feeds do Adafruit IO (crie com estes mesmos nomes)
Adafruit_MQTT_Publish feedHistorico(&mqtt, AIO_USUARIO "/feeds/historico");
Adafruit_MQTT_Publish feedUltimoGato(&mqtt, AIO_USUARIO "/feeds/ultimo-gato");
Adafruit_MQTT_Publish feedPorcao(&mqtt, AIO_USUARIO "/feeds/porcao");
Adafruit_MQTT_Publish feedStatus(&mqtt, AIO_USUARIO "/feeds/status");

// Um unico feed com o resumo do dia de todos os gatos,
// ex.: "Aurora 40.0g/2x | Bento 15.0g/1x | Cristal 0.0g/0x"
Adafruit_MQTT_Publish feedResumoHoje(&mqtt, AIO_USUARIO "/feeds/resumo-hoje");

// ==========================================
// CADASTRO DOS GATOS
// ==========================================

String uids[MAX_GATOS];

int totalGatos = 0;

String nomes[] = {
  "Aurora",
  "Bento",
  "Cristal"
};

String perfis[] = {
  "Peso normal",
  "Sobrepeso",
  "Filhote"
};

int porcoes[] = {
  20,
  15,
  10
};

// Quantos gatos tem perfil nutricional (calculado sozinho:
// para adicionar um gato, basta incluir nos tres arrays acima)
const int TOTAL_PERFIS = sizeof(porcoes) / sizeof(porcoes[0]);

// ==========================================
// ESTADOS DO SISTEMA
// ==========================================

bool modoCadastro = false;

bool balancaDisponivel = false;

unsigned long ultimaMovimentacao = 0;

unsigned long ultimoClique = 0;

unsigned long ultimaLeitura = 0;

bool estadoAnteriorBotao = HIGH;

String ultimoUID = "";

// Consumo de hoje de cada gato, na mesma ordem de uids[]
// (salvo na memoria permanente para sobreviver ao Deep Sleep)
float gramasHoje[MAX_GATOS];

int refeicoesHoje[MAX_GATOS];

unsigned long ultimaTentativaMQTT = 0;

unsigned long ultimoPing = 0;

unsigned long ultimaChecagemDia = 0;

// ==========================================
// CONEXAO WIFI / MQTT
// ==========================================

void conectarWiFi() {

  Serial.print("Conectando ao WiFi");

  WiFi.mode(WIFI_STA);

  // Canal 6 deixa a conexao mais rapida no Wokwi
  WiFi.begin(WIFI_SSID, WIFI_SENHA, 6);

  unsigned long inicio = millis();

  // Espera no maximo 10 s: o comedouro precisa
  // funcionar mesmo sem internet
  while (WiFi.status() != WL_CONNECTED &&
         millis() - inicio < 10000) {

    delay(250);

    Serial.print(".");

  }

  if (WiFi.status() == WL_CONNECTED) {

    Serial.println(" conectado!");

  } else {

    Serial.println(" falhou. Seguindo offline.");

  }

  // Relogio pela internet (NTP)
  configTime(FUSO_HORARIO, 0, "pool.ntp.org", "time.google.com");

  if (WiFi.status() == WL_CONNECTED) {

    struct tm t;

    // Espera ate 5 s pelo primeiro acerto do relogio
    if (getLocalTime(&t, 5000)) {

      Serial.println("Relogio sincronizado: " + horaAtual());

    }

  }

}

bool mqttPronto() {

  return WiFi.status() == WL_CONNECTED && mqtt.connected();

}

void publicarTexto(Adafruit_MQTT_Publish &feed, const String &texto) {

  if (!mqttPronto()) {

    Serial.println("[IO] Offline, nao enviado: " + texto);

    return;

  }

  if (!feed.publish(texto.c_str())) {

    Serial.println("[IO] Falha ao enviar: " + texto);

  }

}

void publicarNumero(Adafruit_MQTT_Publish &feed, float valor) {

  if (!mqttPronto()) {

    return;

  }

  if (!feed.publish(valor, 1)) {

    Serial.println("[IO] Falha ao enviar numero.");

  }

}

String textoStatus() {

  return modoCadastro ? "Modo cadastro" : "Modo normal";

}

// Envia o estado atual sempre que conecta,
// para o dashboard nunca ficar desatualizado
void publicarResumo() {

  publicarTexto(feedStatus, textoStatus());

  publicarTexto(feedResumoHoje, montarResumoDia());

}

// Tenta (re)conectar ao Adafruit IO sem travar o comedouro
void manterMQTT(bool forcar) {

  if (WiFi.status() != WL_CONNECTED) {

    return;

  }

  if (mqtt.connected()) {

    // Mantem a conexao viva
    if (millis() - ultimoPing > 60000) {

      ultimoPing = millis();

      if (!mqtt.ping()) {

        mqtt.disconnect();

      }

    }

    return;

  }

  if (!forcar && millis() - ultimaTentativaMQTT < 15000) {

    return;

  }

  ultimaTentativaMQTT = millis();

  Serial.println("Conectando ao Adafruit IO...");

  int8_t erro = mqtt.connect();

  if (erro != 0) {

    Serial.print("[IO] Erro: ");

    Serial.println(mqtt.connectErrorString(erro));

    mqtt.disconnect();

    return;

  }

  Serial.println("Adafruit IO conectado!");

  ultimoPing = millis();

  publicarResumo();

}

// ==========================================
// DATA E HORA
// ==========================================

String horaAtual() {

  struct tm t;

  if (!getLocalTime(&t, 100)) {

    return "--:--";

  }

  char texto[6];

  strftime(texto, sizeof(texto), "%H:%M", &t);

  return String(texto);

}

// Numero unico por dia (ex.: 2026326) ou -1 sem relogio
long codigoDia() {

  struct tm t;

  if (!getLocalTime(&t, 100)) {

    return -1;

  }

  return (t.tm_year + 1900) * 1000L + t.tm_yday;

}

// ==========================================
// TOTAIS DO DIA
// ==========================================

// Chaves na memoria permanente: g0, g1... (gramas)
// e n0, n1... (numero de refeicoes)
String chaveGramas(int indice) {

  return "g" + String(indice);

}

String chaveRefeicoes(int indice) {

  return "n" + String(indice);

}

// Gatos que tem tag cadastrada E perfil nutricional
int gatosComPerfil() {

  return min(totalGatos, TOTAL_PERFIS);

}

void carregarTotaisDia() {

  for (int i = 0; i < MAX_GATOS; i++) {

    gramasHoje[i] = preferencias.getFloat(
      chaveGramas(i).c_str(),
      0
    );

    refeicoesHoje[i] = preferencias.getInt(
      chaveRefeicoes(i).c_str(),
      0
    );

  }

}

void salvarTotaisGato(int indice) {

  preferencias.putFloat(
    chaveGramas(indice).c_str(),
    gramasHoje[indice]
  );

  preferencias.putInt(
    chaveRefeicoes(indice).c_str(),
    refeicoesHoje[indice]
  );

}

// Monta o texto do resumo com todos os gatos,
// ex.: "Aurora 40.0g/2x | Bento 15.0g/1x | Cristal 0.0g/0x"
String montarResumoDia() {

  String resumo = "";

  for (int i = 0; i < gatosComPerfil(); i++) {

    if (i > 0) {

      resumo += " | ";

    }

    resumo += nomes[i] + " " + String(gramasHoje[i], 1) +
              "g/" + String(refeicoesHoje[i]) + "x";

  }

  if (resumo.length() == 0) {

    resumo = "Nenhum gato cadastrado";

  }

  return resumo;

}

// Zera os totais quando vira o dia
void verificarNovoDia() {

  long hoje = codigoDia();

  if (hoje == -1) {

    return;

  }

  if (hoje == preferencias.getLong("dia", -1)) {

    return;

  }

  preferencias.putLong("dia", hoje);

  for (int i = 0; i < MAX_GATOS; i++) {

    gramasHoje[i] = 0;

    refeicoesHoje[i] = 0;

    salvarTotaisGato(i);

  }

  publicarTexto(feedResumoHoje, montarResumoDia());

  Serial.println("Novo dia: totais zerados.");

}

void registrarAlimentacao(int indice, float gramas, int porcao) {

  verificarNovoDia();

  gramasHoje[indice] += gramas;

  refeicoesHoje[indice]++;

  salvarTotaisGato(indice);

  String hora = horaAtual();

  String linha = hora + " | " + nomes[indice] +
                 " | " + String(gramas, 1) + " g";

  if (gramas < porcao) {

    linha += " (tempo limite)";

  }

  publicarTexto(feedHistorico, linha);

  publicarTexto(feedUltimoGato, nomes[indice] + " (" + hora + ")");

  publicarNumero(feedPorcao, gramas);

  publicarTexto(feedResumoHoje, montarResumoDia());

}

void registrarAlerta(const String &mensagem) {

  publicarTexto(feedHistorico, horaAtual() + " | " + mensagem);

}

// ==========================================
// CONTROLE DOS LEDS
// ==========================================

void atualizarLEDs() {

  digitalWrite(LED_SLEEP, LOW);

  if (modoCadastro) {

    digitalWrite(LED_RED, HIGH);
    digitalWrite(LED_GREEN, LOW);

  } else {

    digitalWrite(LED_RED, LOW);
    digitalWrite(LED_GREEN, HIGH);

  }
}

// ==========================================
// PERSISTENCIA DAS TAGS
// ==========================================

void salvarTags() {

  String dados = "";

  for (int i = 0; i < totalGatos; i++) {

    if (i > 0) {
      dados += ",";
    }

    dados += uids[i];
  }

  preferencias.putString("uids", dados);

}

// ==========================================
// CARREGAR TAGS
// ==========================================

void carregarTags() {

  String dados = preferencias.getString(
    "uids",
    "01020304,11223344,55667788"
  );

  totalGatos = 0;

  int inicio = 0;

  while (inicio < dados.length() &&
         totalGatos < MAX_GATOS) {

    int separador = dados.indexOf(',', inicio);

    String uid;

    if (separador == -1) {

      uid = dados.substring(inicio);

      inicio = dados.length();

    } else {

      uid = dados.substring(inicio, separador);

      inicio = separador + 1;

    }

    if (uid.length() > 0) {

      uids[totalGatos] = uid;

      totalGatos++;

    }

  }

  Serial.print("Tags carregadas: ");
  Serial.println(totalGatos);

}

// ==========================================
// CONVERTER UID
// ==========================================

String obterUID() {

  String uid = "";

  for (byte i = 0; i < leitor.uid.size; i++) {

    if (leitor.uid.uidByte[i] < 0x10) {

      uid += "0";

    }

    uid += String(leitor.uid.uidByte[i], HEX);

  }

  uid.toUpperCase();

  return uid;

}

// ==========================================
// LOCALIZAR GATO
// ==========================================

int localizarGato(String uid) {

  for (int i = 0; i < totalGatos; i++) {

    if (uids[i] == uid) {

      return i;

    }

  }

  return -1;

}

// ==========================================
// MODO CADASTRO
// ==========================================

void cadastrarTag(String uid) {

  if (localizarGato(uid) != -1) {

    Serial.println("Essa tag ja esta cadastrada!");

    return;

  }

  if (totalGatos >= MAX_GATOS) {

    Serial.println("Limite de cadastros atingido!");

    return;

  }

  uids[totalGatos] = uid;

  totalGatos++;

  salvarTags();

  Serial.println("NOVA TAG CADASTRADA!");

  Serial.print("UID: ");
  Serial.println(uid);

  Serial.println(
    "Associe essa tag a um gato no cadastro nutricional."
  );

}

// ==========================================
// LIBERAR RACAO COM HX711
// ==========================================

// Retorna os gramas liberados, ou -1 se a balanca falhar
float liberarRacao(int quantidade) {

  Serial.println("\n========================");

  Serial.println("INICIANDO DOSAGEM");

  // Verifica a disponibilidade da balanca
  if (!balancaDisponivel || !balanca.is_ready()) {

    Serial.println("ERRO: balanca indisponivel!");

    Serial.println("Alimentacao cancelada.");

    return -1;

  }

  // Registra o peso inicial
  float pesoInicial = balanca.get_units(10);

  Serial.print("Peso inicial: ");

  Serial.print(pesoInicial, 1);

  Serial.println(" g");

  Serial.print("Porcao desejada: ");

  Serial.print(quantidade);

  Serial.println(" g");

  // Abre a comporta
  servo.write(SERVO_ABERTO);

  Serial.println("Comporta aberta.");

  unsigned long inicio = millis();

  float pesoLiberado = 0;

  bool dosagemConcluida = false;

  // Aguarda a quantidade desejada
  while (millis() - inicio < TEMPO_MAXIMO_DOSAGEM) {

    if (balanca.is_ready()) {

      float pesoAtual = balanca.get_units(5);

      // Calcula a diferenca de peso
      pesoLiberado = pesoAtual - pesoInicial;

      if (pesoLiberado < 0) {

        pesoLiberado = 0;

      }

      Serial.print("Racao liberada: ");

      Serial.print(pesoLiberado, 1);

      Serial.println(" g");

      // Verifica se atingiu a porcao
      if (pesoLiberado >= quantidade) {

        dosagemConcluida = true;

        break;

      }

    }

    delay(100);

  }

  // Fecha a comporta
  servo.write(SERVO_FECHADO);

  Serial.println("Comporta fechada.");

  if (dosagemConcluida) {

    Serial.println("DOSAGEM CONCLUIDA!");

  } else {

    Serial.println("ATENCAO: tempo limite atingido!");

  }

  Serial.print("Quantidade liberada: ");

  Serial.print(pesoLiberado, 1);

  Serial.println(" g");

  Serial.println("========================");

  return pesoLiberado;

}

// ==========================================
// IDENTIFICAR GATO
// ==========================================

void identificarGato(String uid) {

  int indice = localizarGato(uid);

  if (indice == -1) {

    Serial.println("Gato nao cadastrado!");

    registrarAlerta("Tag desconhecida: " + uid);

    return;

  }

  Serial.println("\n========================");

  // Os primeiros gatos (um por posicao nos arrays
  // nomes/perfis/porcoes) possuem perfis nutricionais

  if (indice < TOTAL_PERFIS) {

    Serial.print("Gato: ");

    Serial.println(nomes[indice]);

    Serial.print("Perfil: ");

    Serial.println(perfis[indice]);

    Serial.print("Porcao configurada: ");

    Serial.print(porcoes[indice]);

    Serial.println(" g");

    float liberado = liberarRacao(porcoes[indice]);

    if (liberado < 0) {

      registrarAlerta(
        nomes[indice] + " nao comeu: balanca indisponivel"
      );

    } else {

      registrarAlimentacao(indice, liberado, porcoes[indice]);

    }

    Serial.println("Processo de alimentacao finalizado.");

  } else {

    Serial.println("TAG IDENTIFICADA!");

    Serial.println(
      "Cadastro nutricional pendente."
    );

    Serial.println(
      "Alimentacao bloqueada ate configurar o gato."
    );

    registrarAlerta("Tag sem perfil nutricional: " + uid);

  }

  Serial.println("========================");

}

// ==========================================
// LEITURA RFID
// ==========================================

void verificarRFID() {

  if (!leitor.PICC_IsNewCardPresent()) {

    return;

  }

  if (!leitor.PICC_ReadCardSerial()) {

    return;

  }

  String uid = obterUID();

  if (uid == ultimoUID &&
      millis() - ultimaLeitura < 3000) {

    leitor.PICC_HaltA();

    leitor.PCD_StopCrypto1();

    return;

  }

  ultimoUID = uid;

  ultimaLeitura = millis();

  // Reinicia o tempo de inatividade
  ultimaMovimentacao = millis();

  Serial.print("UID detectado: ");

  Serial.println(uid);

  if (modoCadastro) {

    cadastrarTag(uid);

  } else {

    identificarGato(uid);

  }

  leitor.PICC_HaltA();

  leitor.PCD_StopCrypto1();

}

// ==========================================
// BOTAO
// ==========================================

void verificarBotao() {

  bool estadoAtual = digitalRead(BUTTON_PIN);

  if (estadoAnteriorBotao == HIGH &&
      estadoAtual == LOW &&
      millis() - ultimoClique > DEBOUNCE) {

    ultimoClique = millis();

    modoCadastro = !modoCadastro;

    atualizarLEDs();

    if (modoCadastro) {

      Serial.println("\nMODO CADASTRO ATIVADO");

      Serial.println("Aproxime uma nova tag RFID.");

    } else {

      Serial.println("\nMODO NORMAL ATIVADO");

    }

    publicarTexto(feedStatus, textoStatus());

  }

  estadoAnteriorBotao = estadoAtual;

}

// ==========================================
// DEEP SLEEP
// ==========================================

void entrarDeepSleep() {

  Serial.println("\nSem movimento por 60 segundos.");

  Serial.println("Entrando em Deep Sleep...");

  // Avisa o dashboard e desconecta
  publicarTexto(feedStatus, "Dormindo");

  delay(200);

  mqtt.disconnect();

  WiFi.disconnect(true);

  // Fecha a comporta
  servo.write(SERVO_FECHADO);

  delay(300);

  // Desliga LEDs de funcionamento
  digitalWrite(LED_RED, LOW);

  digitalWrite(LED_GREEN, LOW);

  // Acende LED azul
  pinMode(LED_SLEEP, OUTPUT);

  digitalWrite(LED_SLEEP, HIGH);

  // Mantem LED azul aceso durante o sono
  gpio_hold_en(GPIO_NUM_14);

  gpio_deep_sleep_hold_en();

  // Configura o PIR como fonte de despertar
  esp_sleep_enable_ext0_wakeup(
    GPIO_NUM_33,
    1
  );

  Serial.flush();

  esp_deep_sleep_start();

}

// ==========================================
// SETUP
// ==========================================

void setup() {

  Serial.begin(115200);

  // Libera a retencao do LED azul
  gpio_deep_sleep_hold_dis();

  gpio_hold_dis(GPIO_NUM_14);

  // Configura LEDs
  pinMode(LED_RED, OUTPUT);

  pinMode(LED_GREEN, OUTPUT);

  pinMode(LED_SLEEP, OUTPUT);

  // Configura botao
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // Configura PIR
  pinMode(PIR_PIN, INPUT);

  digitalWrite(LED_SLEEP, LOW);

  // Inicializa modo normal
  modoCadastro = false;

  atualizarLEDs();

  // ========================================
  // MEMORIA PERMANENTE
  // ========================================

  preferencias.begin("smartpet", false);

  carregarTags();

  carregarTotaisDia();

  // ========================================
  // SERVO
  // ========================================

  servo.setPeriodHertz(50);

  servo.attach(SERVO_PIN, 500, 2400);

  servo.write(SERVO_FECHADO);

  // ========================================
  // BALANCA HX711
  // ========================================

  balanca.begin(HX_DT, HX_SCK);

  balanca.set_scale(FATOR_CALIBRACAO);

  Serial.println("Inicializando balanca...");

  unsigned long inicioBalanca = millis();

  // Aguarda o sensor responder por ate 3 segundos
  while (!balanca.is_ready() &&
         millis() - inicioBalanca < 3000) {

    delay(50);

  }

  if (balanca.is_ready()) {

    balanca.tare();

    balancaDisponivel = true;

    Serial.println("Balanca iniciada!");

  } else {

    balancaDisponivel = false;

    Serial.println("ERRO: HX711 nao encontrado!");

  }

  // ========================================
  // RFID
  // ========================================

  SPI.begin(18, 19, 23, RFID_SS);

  leitor.PCD_Init();

  // ========================================
  // WIFI + DASHBOARD
  // ========================================

  conectarWiFi();

  // Zera os totais se o dia mudou enquanto dormia
  verificarNovoDia();

  manterMQTT(true);

  // ========================================
  // INICIALIZACAO
  // ========================================

  ultimaMovimentacao = millis();

  Serial.println("\n============================");

  Serial.println("     SMARTPET FEEDER");

  Serial.println("============================");

  Serial.println("Sistema iniciado!");

  Serial.println("Modo normal ativo.");

  Serial.print("Gatos cadastrados: ");

  Serial.println(totalGatos);

  if (digitalRead(PIR_PIN) == HIGH) {

    ultimaMovimentacao = millis();

  }

}

// ==========================================
// LOOP
// ==========================================

void loop() {

  // ========================================
  // DETECCAO DE MOVIMENTO
  // ========================================

  if (digitalRead(PIR_PIN) == HIGH) {

    ultimaMovimentacao = millis();

  }

  // ========================================
  // BOTAO
  // ========================================

  verificarBotao();

  // ========================================
  // DASHBOARD
  // ========================================

  manterMQTT(false);

  if (millis() - ultimaChecagemDia > 60000) {

    ultimaChecagemDia = millis();

    verificarNovoDia();

  }

  // ========================================
  // RFID
  // ========================================

  if (modoCadastro ||
      millis() - ultimaMovimentacao < TEMPO_SEM_MOVIMENTO) {

    verificarRFID();

  }

  // ========================================
  // DEEP SLEEP
  // ========================================

  if (!modoCadastro &&
      digitalRead(PIR_PIN) == LOW &&
      millis() - ultimaMovimentacao >= TEMPO_SEM_MOVIMENTO) {

    entrarDeepSleep();

  }

  delay(20);

}
