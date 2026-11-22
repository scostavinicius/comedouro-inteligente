# Comedouro Inteligente com ESP32 (SmartPet Feeder)

Comedouro automático para gatos. Cada gato usa uma tag RFID na coleira; o comedouro reconhece o gato, libera a porção de ração do perfil dele (controlada por uma balança) e envia o histórico para um dashboard no **Adafruit IO**. Quando não há movimento por 60 s, o ESP32 entra em *deep sleep* e acorda pelo sensor de presença.

> Projeto em desenvolvimento: o circuito foi feito no simulador [Wokwi](https://wokwi.com/projects/480681500123448321) e deve mudar ao ser montado na prática.

## Funcionalidades

- Identificação do gato por **RFID** (MFRC522).
- Dosagem por peso: o **servo** abre a comporta e a **balança (HX711)** fecha quando atinge a porção do gato (limite de 20 s).
- **Modo cadastro** pelo botão, para registrar novas tags (salvas na memória flash).
- **Economia de energia**: deep sleep após 60 s sem movimento, despertar pelo **sensor PIR**.
- **Dashboard no Adafruit IO**: histórico de alimentações com horário, último gato, porção liberada, status do comedouro e resumo do dia de cada gato.
- Funciona **offline**: sem internet o comedouro continua alimentando; só o dashboard deixa de atualizar.

## Componentes e ligações

| Componente | Função | Pinos do ESP32 |
|---|---|---|
| MFRC522 (RFID) | Lê a tag do gato | SS=5, SCK=18, MOSI=23, MISO=19, RST=21 |
| Servo | Comporta (0° fechada, 90° aberta) | 13 |
| HX711 + célula de carga 5 kg | Balança | DT=16, SCK=17 |
| Sensor PIR | Presença / despertar | 33 |
| Botão | Alterna modo normal / cadastro | 25 (outro lado no GND) |
| LED verde | Modo normal | 27 |
| LED vermelho | Modo cadastro | 26 |
| LED azul | Dormindo (deep sleep) | 14 |

LEDs com resistor de 220 Ω. Na simulação tudo é alimentado em 3,3 V; **na montagem real o servo e o PIR precisam de 5 V** (servo com fonte própria e GND em comum com o ESP32).

## Estrutura do repositório

| Arquivo | Conteúdo |
|---|---|
| `sketch.ino` | Código do ESP32 |
| `diagram.json` | Circuito do Wokwi |
| `libraries.txt` | Bibliotecas usadas pelo Wokwi |
| `secrets.example.h` | Modelo das credenciais do Adafruit IO |
| `secrets.h` | Suas credenciais (**não versionado**, ver `.gitignore`) |

## Como rodar no Wokwi

1. Crie um projeto ESP32 no Wokwi (ou abra o projeto do grupo) e copie `sketch.ino`, `diagram.json` e `libraries.txt`.
2. Clique no **"+"** ao lado das abas → *New file* → `secrets.h` e cole o conteúdo de `secrets.example.h`, preenchendo seu usuário e sua AIO Key.
3. Inicie a simulação e abra o Serial Monitor. Devem aparecer `conectado!`, `Relogio sincronizado` e `Adafruit IO conectado!`.
4. **Alimentar:** clique no leitor RFID e escolha uma tag. Durante a dosagem, clique na balança e **aumente o peso** para simular a ração caindo.
5. **Cadastrar tag:** aperte o botão (LED vermelho), aproxime a tag nova e aperte de novo.
6. **Deep sleep:** espere 60 s sem acionar o PIR (LED azul acende); clique no PIR para acordar.

No Wokwi a rede é sempre `Wokwi-GUEST`, sem senha. No projeto físico, altere `WIFI_SSID` e `WIFI_SENHA` no início do `sketch.ino`.

## Dashboard no Adafruit IO

1. Crie uma conta em [io.adafruit.com](https://io.adafruit.com) e copie o usuário e a chave (ícone da chave amarela, *My Key*) para o `secrets.h`.
2. Em **Feeds → New Feed**, crie os feeds abaixo com estes nomes exatos:

| Feed | Exemplo de valor | Bloco sugerido |
|---|---|---|
| `historico` | `14:32 \| Aurora \| 20.0 g` (e alertas) | Stream |
| `ultimo-gato` | `Aurora (14:32)` | Text |
| `porcao` | `20.0` | Line Chart |
| `status` | `Modo normal` / `Modo cadastro` / `Dormindo` | Text |
| `resumo-hoje` | `Aurora 40.0g/2x \| Bento 15.0g/1x \| Cristal 0.0g/0x` | Text |

3. Em **Dashboards → New Dashboard**, abra o dashboard, clique na engrenagem → **Create New Block** e adicione um bloco para cada feed.

O resumo do dia é zerado automaticamente à meia-noite (horário de Brasília, obtido via NTP).

Limites do plano grátis: 10 feeds, 30 envios por minuto e 30 dias de histórico. Cada alimentação usa 4 envios.

## Gatos cadastrados

Os perfis ficam nos arrays `nomes`, `perfis` e `porcoes` do `sketch.ino`, na mesma ordem das tags salvas:

| Posição | Gato | Perfil | Porção | Tag padrão |
|---|---|---|---|---|
| 0 | Aurora | Peso normal | 20 g | `01020304` |
| 1 | Bento | Sobrepeso | 15 g | `11223344` |
| 2 | Cristal | Filhote | 10 g | `55667788` |

Para adicionar um gato, inclua uma linha nos três arrays; o resto do código e o dashboard se ajustam sozinhos. Tags cadastradas pelo botão sem perfil correspondente são reconhecidas, mas a alimentação fica bloqueada.

## Segurança

- Nunca coloque a AIO Key no `sketch.ino` nem faça commit do `secrets.h`.
- Projetos salvos no Wokwi podem ser vistos por quem tiver o link; se a chave for exposta, use **Regenerate Key** no Adafruit IO.

## Próximos passos

- Juntar os dados de cada gato em uma `struct` e salvar nome/porção na flash, permitindo cadastro completo sem alterar o código.
- Cadastrar gatos pelo dashboard (feed `config` + bloco de texto, recebido via `Adafruit_MQTT_Subscribe`).
- Tornar a dosagem não bloqueante (hoje o sistema fica até 20 s parado dosando).
- Adaptar alimentação (5 V) e calibração da balança para o circuito físico.
