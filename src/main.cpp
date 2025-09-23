#include <Arduino.h>
#include <Keypad.h>
#include <Preferences.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>

// ====== LIGA/DESLIGA CSV LOCAL (LittleFS) ======
#define USE_LOCAL_CSV 0
#if USE_LOCAL_CSV
#include <FS.h>
#include <LittleFS.h>
#endif

// ====== AJUSTES DE PERFORMANCE ======
#define SUPA_BOOT_PING 0       // 0 = não envia "boot/online" ao iniciar (mais rápido)
#define SUPA_RETRIES 1         // só 1 tentativa
#define SUPA_CONN_TIMEOUT 8000 // timeout de conexão (ms)

// ============ CONFIG Wi-Fi / NTP / SUPABASE ============
const char *WIFI_SSID = "Wokwi-GUEST";
const char *WIFI_PASS = "";

const char *NTP_SERVER = "pool.ntp.org";
const long GMT_OFFSET_SEC = -3 * 3600;
const int DAYLIGHT_OFFSET_SEC = 0;

const char *SUPABASE_URL = "https://tqawdemzrlfzzkyhaecz.supabase.co/rest/v1/acessos";
const char *SUPABASE_HOST = "tqawdemzrlfzzkyhaecz.supabase.co";
const char *SUPABASE_KEY =
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6InRxYXdkZW16cmxmenpreWhhZWN6Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3NTg0NzIzNzAsImV4cCI6MjA3NDA0ODM3MH0.dQaffEeK9DZ545D_c5z2nzIhLMJdK9wmmb3QyjhhGTc";

static inline bool haveCloud()
{
  return WiFi.status() == WL_CONNECTED &&
         strlen(SUPABASE_URL) > 0 &&
         strlen(SUPABASE_KEY) > 0;
}

// ============ PINOS ============
const int PINO_RELE = 25;
const int LED_VERDE_PIN = 13; // liberado
const int LED_VERM_PIN = 15;  // bloqueado
const int PINO_BOTAO = 26;    // << push-button físico

// Teclado 4x4
const byte LINHAS = 4, COLUNAS = 4;
char teclas[LINHAS][COLUNAS] = {
    {'1', '2', '3', 'A'},
    {'4', '5', '6', 'B'},
    {'7', '8', '9', 'C'},
    {'*', '0', '#', 'D'}};
byte pinosLinhas[LINHAS] = {19, 18, 5, 17};
byte pinosColunas[COLUNAS] = {16, 4, 27, 14};
Keypad teclado = Keypad(makeKeymap(teclas), pinosLinhas, pinosColunas, LINHAS, COLUNAS);

// LCD I2C
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ============ LÓGICA ============
const bool RELE_ATIVO_ALTO = true;
const unsigned long TEMPO_ABERTO_MS = 5000UL;
const unsigned long TEMPO_BLOQUEIO_MS = 30000UL;
const int MAX_TENTATIVAS = 5;

const char *MASTER_PADRAO = "9999";
#define MAX_CREDS 50

// ============ ESTADO ============
Preferences prefs;

struct Cred
{
  String pin;
  String nome;
};
Cred cred[MAX_CREDS];
int qtdCreds = 0;

String masterCode = "";
String entrada = "";     // buffer SENHA/SSM
String ultimoPinOk = ""; // guarda PIN após 1ª etapa para log
bool fechaduraAberta = false;
unsigned long tempoFechar = 0;

int tentativasErradas = 0;
unsigned long tempoBloqueio = 0;

bool modoAdmin = false;
bool aguardandoMaster = false;
bool acaoAdminPendente = false;
char tipoAcaoAdmin = 0; // 'C','D','M'
bool aguardandoSSM = false;

// ===== TLS warm-up: ajuda a evitar erro -80 no Wokwi =====
void tlsWarmup()
{
  WiFiClientSecure s;
  s.setInsecure();
  s.setTimeout(SUPA_CONN_TIMEOUT);
  if (s.connect(SUPABASE_HOST, 443))
  {
    s.stop();
    delay(150);
  }
}

// ============ PROTÓTIPOS ============
void telaInicial();
void telaPedeSenha();
void telaMostraSenha();
void telaPedeSSM();
void telaMostraSSM();
void telaMsg(const char *, const char *);
void atualizaLeds();
void abreFechadura();
void fechaFechadura();
void trataTecla(char k);
void checaBotao(); // << novo

void carregarCredenciais();
void salvarCredenciais();
bool adicionarPIN(const String &, const String &);
bool removerPIN(const String &);
bool senhaValida(const String &);
String nomePorPIN(const String &);
void listarCredenciaisSerial();

void initFS();
void exportCSVSerial();
void clearCSV();
void logEvento(const String &, const String &, const String &);

String timestampAgora();
void initWiFiNTP();
void processaSerialComandos();
bool supabasePOST(const String &ts, const String &usuario, const String &pin, const String &ssm, const String &resultado);

// ============ SETUP ============
void setup()
{
  Serial.begin(115200);

  pinMode(PINO_RELE, OUTPUT);
  pinMode(LED_VERDE_PIN, OUTPUT);
  pinMode(LED_VERM_PIN, OUTPUT);
  pinMode(PINO_BOTAO, INPUT_PULLUP); // botão entre GPIO26 e GND
  digitalWrite(PINO_RELE, RELE_ATIVO_ALTO ? LOW : HIGH);

  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  telaInicial();

  initFS();
  prefs.begin("fechadura", false);

  if (!prefs.isKey("master"))
    prefs.putString("master", MASTER_PADRAO);
  masterCode = prefs.getString("master", MASTER_PADRAO);

  if (!prefs.isKey("cred"))
  {
    prefs.putString("cred", "2222|Leonardo,1111|Juliano,3333|Carlos,4444|Jonatas,5555|Ricardo");
  }
  carregarCredenciais();

  atualizaLeds();
  telaPedeSenha();

  initWiFiNTP();
  tlsWarmup();

#if SUPA_BOOT_PING
  if (haveCloud())
    supabasePOST(timestampAgora(), "boot", "", "", "online");
#endif

  Serial.println("=== FECHADURA ESP32 (Dual) ===");
  Serial.println("Fluxo: SENHA + #  ->  SSM (2-6) + #");
  Serial.println("Admin: A + master + #");
  Serial.println("ADMIN: C=ADD PIN, D=DEL PIN, A=MASTER, *=LISTAR, B=SAIR, 0=EXPORT CSV, 9=APAGAR LOGS");
  Serial.println("Serial: SET <pin> <nome>");
}

// ============ LOOP ============
void loop()
{
  unsigned long agora = millis();

  if (fechaduraAberta && agora >= tempoFechar)
  {
    fechaFechadura();
    telaPedeSenha();
  }
  if (tempoBloqueio && agora >= tempoBloqueio)
  {
    tempoBloqueio = 0;
    tentativasErradas = 0;
    telaMsg("Bloqueio acabou", "Tente novamente");
    delay(900);
    telaPedeSenha();
  }

  checaBotao(); // << lê o botão físico
  char k = teclado.getKey();
  if (k)
    trataTecla(k);

  processaSerialComandos();
}

// ============ BOTÃO FÍSICO (GPIO26) ============
void checaBotao()
{
  // debouncing + detecção de borda
  static int lastRead = HIGH, stable = HIGH;
  static unsigned long t = 0;
  const unsigned long DEBOUNCE_MS = 30;

  int r = digitalRead(PINO_BOTAO);
  if (r != lastRead)
  {
    lastRead = r;
    t = millis();
  }
  if ((millis() - t) > DEBOUNCE_MS && r != stable)
  {
    stable = r;
    if (stable == LOW)
    { // botão pressionado
      if (!fechaduraAberta && tempoBloqueio == 0)
      {
        abreFechadura();
        logEvento("sucesso_btn", "BOTAO", "");
        telaMsg("ABERTO (BOTAO)", "");
      }
    }
  }
}

// ============ TECLADO ============
void trataTecla(char k)
{
  if (tempoBloqueio)
  {
    telaMsg("BLOQUEADO", "Aguarde...");
    return;
  }

  if (k == '*' && !modoAdmin && !aguardandoMaster && !acaoAdminPendente)
  {
    entrada = "";
    (!aguardandoSSM) ? telaPedeSenha() : telaPedeSSM();
    return;
  }

  if (k == 'A' && !modoAdmin && !aguardandoMaster)
  {
    aguardandoMaster = true;
    entrada = "";
    telaMsg("ADMIN", "Master + #");
    return;
  }

  if (aguardandoMaster)
  {
    if (k == '#')
    {
      if (entrada == masterCode)
      {
        modoAdmin = true;
        aguardandoMaster = false;
        entrada = "";
        telaMsg("ADMIN ATIVO", "C/D/A/*/B 0/9");
      }
      else
      {
        telaMsg("Master incorreto", "Saindo...");
        aguardandoMaster = false;
        entrada = "";
        delay(800);
        telaPedeSenha();
      }
      return;
    }
    else if (k >= '0' && k <= '9')
    {
      if (entrada.length() < 8)
        entrada += k;
      lcd.setCursor(0, 1);
      lcd.print("Master: ");
      for (int i = 0; i < entrada.length(); i++)
        lcd.print('*');
      return;
    }
  }

  if (modoAdmin)
  {
    if (k == 'B' && !acaoAdminPendente)
    {
      modoAdmin = false;
      entrada = "";
      telaMsg("ADMIN", "Saindo...");
      delay(700);
      telaPedeSenha();
      return;
    }
    if (k == '*' && !acaoAdminPendente)
    {
      listarCredenciaisSerial();
      telaMsg("Listou no Serial", "");
      delay(700);
      telaMsg("ADMIN", "C/D/A/*/B 0/9");
      return;
    }
    if (k == '0' && !acaoAdminPendente)
    {
      exportCSVSerial();
      telaMsg("CSV no Serial", "");
      delay(700);
      telaMsg("ADMIN", "C/D/A/*/B 0/9");
      return;
    }
    if (k == '9' && !acaoAdminPendente)
    {
      clearCSV();
      telaMsg("Logs apagados", "");
      delay(600);
      telaMsg("ADMIN", "C/D/A/*/B 0/9");
      return;
    }

    if (acaoAdminPendente)
    {
      if (k == '#')
      {
        if (tipoAcaoAdmin == 'C')
        {
          if (entrada.length() >= 4 && entrada.length() <= 8)
          {
            String pin = entrada, nome = "User_" + pin;
            telaMsg(adicionarPIN(pin, nome) ? "PIN adicionado" : "Falha ao adicionar", "Padrao: User_PIN");
          }
          else
            telaMsg("PIN invalido", "4-8 digitos");
        }
        else if (tipoAcaoAdmin == 'D')
        {
          telaMsg(removerPIN(entrada) ? "PIN removido" : "Nao encontrado", "");
        }
        else if (tipoAcaoAdmin == 'M')
        {
          if (entrada.length() >= 4 && entrada.length() <= 8)
          {
            masterCode = entrada;
            prefs.putString("master", masterCode);
            telaMsg("MASTER atualizado", "");
          }
          else
            telaMsg("MASTER invalido", "4-8 digitos");
        }
        acaoAdminPendente = false;
        tipoAcaoAdmin = 0;
        entrada = "";
        delay(800);
        telaMsg("ADMIN", "C/D/A/*/B 0/9");
      }
      else if (k >= '0' && k <= '9')
      {
        if (entrada.length() < 8)
          entrada += k;
        lcd.setCursor(0, 1);
        lcd.print("Valor: ");
        for (int i = 0; i < entrada.length(); i++)
          lcd.print('*');
      }
      return;
    }

    if (k == 'C')
    {
      acaoAdminPendente = true;
      tipoAcaoAdmin = 'C';
      entrada = "";
      telaMsg("ADD PIN", "Novo + #");
      return;
    }
    if (k == 'D')
    {
      acaoAdminPendente = true;
      tipoAcaoAdmin = 'D';
      entrada = "";
      telaMsg("REMOVER PIN", "PIN + #");
      return;
    }
    if (k == 'A')
    {
      acaoAdminPendente = true;
      tipoAcaoAdmin = 'M';
      entrada = "";
      telaMsg("MASTER", "Novo + #");
      return;
    }
    return;
  }

  // ===== MODO NORMAL =====
  if (!aguardandoSSM)
  { // SENHA
    if (k == '#')
    {
      if (senhaValida(entrada))
      {
        ultimoPinOk = entrada;
        aguardandoSSM = true;
        entrada = "";
        telaPedeSSM();
      }
      else
      {
        tentativasErradas++;
        logEvento("senha_incorreta", entrada, "");
        telaMsg("Senha incorreta", "Tente novamente");
        entrada = "";
        if (tentativasErradas >= MAX_TENTATIVAS)
        {
          tempoBloqueio = millis() + TEMPO_BLOQUEIO_MS;
          logEvento("bloqueio", "", "");
          telaMsg("Muitas tentativas", "BLOQUEADO");
        }
        else
        {
          delay(700);
          telaPedeSenha();
        }
      }
      return;
    }
    if (k >= '0' && k <= '9')
    {
      if (entrada.length() < 8)
        entrada += k;
      telaMostraSenha();
    }
    return;
  }

  // SSM
  if (aguardandoSSM)
  {
    if (k == '#')
    {
      if (entrada.length() >= 2 && entrada.length() <= 6)
      {
        abreFechadura();
        logEvento("sucesso", ultimoPinOk, entrada);
        telaMsg("ACESSO LIBERADO", "");
      }
      else
      {
        logEvento("ssm_invalido", ultimoPinOk, entrada);
        telaMsg("SSM invalido", "Min 2 digitos");
        delay(800);
        telaPedeSSM();
      }
      entrada = "";
      aguardandoSSM = false;
      return;
    }
    if (k >= '0' && k <= '9')
    {
      if (entrada.length() < 6)
        entrada += k;
      telaMostraSSM();
    }
    if (k == '*')
    {
      entrada = "";
      telaPedeSSM();
    }
    return;
  }
}

// ============ UI (LCD) ============
void telaInicial()
{
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Fechadura I2C");
  lcd.setCursor(0, 1);
  lcd.print("Iniciando...");
  delay(700);
}
void telaPedeSenha()
{
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Digite a SENHA");
  lcd.setCursor(0, 1);
  lcd.print("Senha: ");
}
void telaMostraSenha()
{
  lcd.setCursor(7, 1);
  int n = entrada.length();
  for (int i = 0; i < n && i < 8; i++)
    lcd.print('*');
  for (int i = n; i < 8; i++)
    lcd.print(' ');
}
void telaPedeSSM()
{
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Senha OK!");
  lcd.setCursor(0, 1);
  lcd.print("SSM: ");
}
void telaMostraSSM()
{
  lcd.setCursor(5, 1);
  int n = entrada.length();
  for (int i = 0; i < n && i < 6; i++)
    lcd.print('*');
  for (int i = n; i < 6; i++)
    lcd.print(' ');
}
void telaMsg(const char *l1, const char *l2)
{
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(l1);
  lcd.setCursor(0, 1);
  lcd.print(l2);
}

// ============ CREDENCIAIS ============
void carregarCredenciais()
{
  String csv = prefs.getString("cred", "2222|Leonardo,1111|Juliano,3333|Carlos,4444|Jonatas,5555|Ricardo");
  qtdCreds = 0;
  int start = 0;
  while (start < csv.length() && qtdCreds < MAX_CREDS)
  {
    int comma = csv.indexOf(',', start);
    if (comma == -1)
      comma = csv.length();
    String item = csv.substring(start, comma);
    item.trim();
    int bar = item.indexOf('|');
    if (bar > 0)
    {
      String pin = item.substring(0, bar);
      pin.trim();
      String nome = item.substring(bar + 1);
      nome.trim();
      if (pin.length())
      {
        cred[qtdCreds].pin = pin;
        cred[qtdCreds].nome = nome;
        qtdCreds++;
      }
    }
    start = comma + 1;
  }
}
void salvarCredenciais()
{
  String csv = "";
  for (int i = 0; i < qtdCreds; i++)
  {
    if (i)
      csv += ",";
    csv += cred[i].pin + "|" + cred[i].nome;
  }
  prefs.putString("cred", csv);
}
bool adicionarPIN(const String &pin, const String &nome)
{
  if (qtdCreds >= MAX_CREDS)
    return false;
  for (int i = 0; i < qtdCreds; i++)
    if (cred[i].pin == pin)
      return false;
  cred[qtdCreds].pin = pin;
  cred[qtdCreds].nome = nome;
  qtdCreds++;
  salvarCredenciais();
  return true;
}
bool removerPIN(const String &pin)
{
  int idx = -1;
  for (int i = 0; i < qtdCreds; i++)
    if (cred[i].pin == pin)
    {
      idx = i;
      break;
    }
  if (idx == -1)
    return false;
  for (int j = idx; j < qtdCreds - 1; j++)
    cred[j] = cred[j + 1];
  qtdCreds--;
  salvarCredenciais();
  return true;
}
bool senhaValida(const String &s)
{
  for (int i = 0; i < qtdCreds; i++)
    if (cred[i].pin == s)
      return true;
  return false;
}
String nomePorPIN(const String &pin)
{
  for (int i = 0; i < qtdCreds; i++)
    if (cred[i].pin == pin)
      return cred[i].nome;
  return "";
}
void listarCredenciaisSerial()
{
  Serial.println("=== PIN|NOME ===");
  for (int i = 0; i < qtdCreds; i++)
  {
    Serial.print(cred[i].pin);
    Serial.print(" | ");
    Serial.println(cred[i].nome);
  }
}

// ============ FS / CSV ============
void initFS()
{
#if USE_LOCAL_CSV
  if (!LittleFS.begin(true))
  {
    Serial.println("LittleFS falhou. CSV desativado.");
    return;
  }
  if (!LittleFS.exists("/logs.csv"))
  {
    File f = LittleFS.open("/logs.csv", FILE_WRITE);
    if (f)
    {
      f.println("timestamp,usuario,pin,ssm,resultado");
      f.close();
    }
  }
#else
  // CSV desativado no Wokwi
#endif
}
void exportCSVSerial()
{
#if USE_LOCAL_CSV
  File f = LittleFS.open("/logs.csv", FILE_READ);
  if (!f)
  {
    Serial.println("Sem logs.");
    return;
  }
  Serial.println("----- INICIO CSV -----");
  while (f.available())
    Serial.write(f.read());
  Serial.println("----- FIM CSV -----");
  f.close();
#else
  Serial.println("CSV desativado.");
#endif
}
void clearCSV()
{
#if USE_LOCAL_CSV
  LittleFS.remove("/logs.csv");
  File f = LittleFS.open("/logs.csv", FILE_WRITE);
  if (f)
  {
    f.println("timestamp,usuario,pin,ssm,resultado");
    f.close();
  }
#else
  Serial.println("CSV desativado.");
#endif
}
void logEvento(const String &resultado, const String &pin, const String &ssm)
{
  String ts = timestampAgora();
  String usuario = nomePorPIN(pin);
#if USE_LOCAL_CSV
  File f = LittleFS.open("/logs.csv", FILE_APPEND);
  if (f)
  {
    f.println(ts + "," + usuario + "," + pin + "," + ssm + "," + resultado);
    f.close();
  }
#endif
  if (haveCloud())
    supabasePOST(ts, usuario, pin, ssm, resultado);
}

// ============ Wi-Fi / NTP ============
void initWiFiNTP()
{
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Conectando Wi-Fi");
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 8000)
  {
    Serial.print(".");
    delay(300);
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED)
  {
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
    for (int i = 0; i < 15; i++)
    {
      struct tm ti;
      if (getLocalTime(&ti))
        break;
      delay(100);
    }
    Serial.println("Wi-Fi/NTP pronto.");
  }
  else
  {
    Serial.println("Wi-Fi falhou -> offline.");
  }
}
String timestampAgora()
{
  if (WiFi.status() == WL_CONNECTED)
  {
    struct tm ti;
    if (getLocalTime(&ti))
    {
      char buf[32];
      strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &ti);
      return String(buf);
    }
  }
  return "offline";
}

// ============ SUPABASE ============
bool supabasePOST(const String &ts, const String &usuario, const String &pin,
                  const String &ssm, const String &resultado)
{
  if (WiFi.status() != WL_CONNECTED)
    return false;

  String body = String("{") +
                "\"timestamp\":\"" + ts + "\"," +
                "\"usuario\":\"" + usuario + "\"," +
                "\"pin\":\"" + pin + "\"," +
                "\"ssm\":\"" + ssm + "\"," +
                "\"resultado\":\"" + resultado + "\"}";

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(SUPA_CONN_TIMEOUT);
  HTTPClient http;
  http.setConnectTimeout(SUPA_CONN_TIMEOUT);
  http.setReuse(false);

  String url = String("https://") + SUPABASE_HOST + "/rest/v1/acessos";
  if (!http.begin(client, url))
  {
    Serial.println("[Supa] begin() falhou");
    http.end();
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  http.addHeader("Prefer", "return=minimal");

  int code = http.POST(body);
  Serial.printf("Supabase HTTP %d (1/1)\n", code);
  http.end();
  return (code == 200 || code == 201 || code == 204);
}

// ============ Relé / LEDs ============
void abreFechadura()
{
  digitalWrite(PINO_RELE, RELE_ATIVO_ALTO ? HIGH : LOW);
  fechaduraAberta = true;
  tempoFechar = millis() + TEMPO_ABERTO_MS;
  atualizaLeds();
}
void fechaFechadura()
{
  digitalWrite(PINO_RELE, RELE_ATIVO_ALTO ? LOW : HIGH);
  fechaduraAberta = false;
  atualizaLeds();
}
void atualizaLeds()
{
  digitalWrite(LED_VERDE_PIN, fechaduraAberta ? HIGH : LOW);
  digitalWrite(LED_VERM_PIN, fechaduraAberta ? LOW : HIGH);
}

// ============ Serial: SET <pin> <nome> ============
void processaSerialComandos()
{
  static String linha = "";
  while (Serial.available())
  {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r')
    {
      if (linha.startsWith("SET "))
      {
        int sp1 = linha.indexOf(' ', 4);
        if (sp1 > 4)
        {
          String pin = linha.substring(4, sp1);
          pin.trim();
          String nome = linha.substring(sp1 + 1);
          nome.trim();
          bool ok = false;
          for (int i = 0; i < qtdCreds; i++)
            if (cred[i].pin == pin)
            {
              cred[i].nome = nome;
              ok = true;
              break;
            }
          if (ok)
          {
            salvarCredenciais();
            Serial.println("Nome atualizado.");
          }
          else
          {
            Serial.println("PIN nao encontrado.");
          }
        }
        else
        {
          Serial.println("Uso: SET <pin> <nome>");
        }
      }
      linha = "";
    }
    else
    {
      linha += c;
    }
  }
}
