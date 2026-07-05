# MP3 Player com ESP32 (e modo WiFi)

> Um tocador de MP3 feito do zero com ESP32 + DFPlayer Mini + display OLED, com tela de player, modo WiFi pra configurar as faixas e o relógio, e um modo de baixo consumo pra não gastar bateria à toa.
>
> **Empresa:** ICMC - USP &nbsp;|&nbsp; **Feito por:** Matheus H. Ferreira &nbsp;|&nbsp; **Rev:** 1.0

---

## Ideia geral

O projeto é basicamente um mp3 player "caseiro": o ESP32 lê as músicas de um cartão SD (através do módulo DFPlayer Mini), toca no fone/caixinha pelo jack P2, e mostra tudo numa telinha OLED — hora, nome da faixa, barra de volume, um "disco" e umas barrinhas de equalizador só de enfeite mesmo. Tem 4 botões (play/pause, next, prev e um botão de modo) e um potenciômetro pra volume.

Além do player em si, dá pra colocar o ESP32 em modo WiFi pra mexer em algumas configurações pelo celular, e também tem um modo de baixo consumo pra economizar a bateria quando não tá usando.

O firmware é organizado como uma pequena máquina de estados de tela (`SCR_PLAYER`, `SCR_NO_SD`, `SCR_WIFI`, `SCR_OFF`), então em qualquer momento o ESP32 sabe exatamente o que deve estar rodando (leitura de botões, animação, servidor web, etc.) de acordo com a tela ativa.

---

## Hardware

- **ESP32 DevKit V1** — é o cérebro de tudo, controla o display, o DFPlayer, lê os botões e sobe o WiFi quando precisa.
- **DFPlayer Mini** — módulo que lê os MP3 do cartão SD e manda o áudio pro jack de fone. Conversa com o ESP32 por serial (Serial2, pinos 16/17), a 9600 bps. O pino **TX do DFPlayer vai direto no RX2 do ESP32**; já o pino **RX do DFPlayer é alimentado pelo TX2 do ESP32 através de um resistor de 1kΩ em série**.
- **Resistor 1kΩ** — colocado em série entre o TX2 do ESP32 e o pino RX do DFPlayer Mini, limitando a corrente que chega nesse pino (ver explicação detalhada na seção de Pinagem, logo abaixo).
- **Display OLED 0.96" (SSD1306, 128×64)** — mostra a tela do player. Ligado por I2C (pinos 32/33), endereço `0x3C`.
- **4 botões** — play/pause, next, prev e o botão de modo (que tem clique único e duplo clique, cada um fazendo uma coisa diferente).
- **Potenciômetro 10k** — controla o volume (leitura analógica, pino ADC).
- **Jack de fone P2** — saída de áudio (Left, Right, Mic, Common).
- **Bateria 18650 + módulo TP4056** — alimenta o circuito inteiro e cuida da recarga via USB.

### Pinagem

| Sinal | GPIO | Observação |
|---|---|---|
| OLED SDA | 32 | I2C |
| OLED SCL | 33 | I2C |
| DFPlayer TX → ESP32 RX2 | 16 | Serial2 — ligação direta, sem resistor |
| DFPlayer RX ← ESP32 TX2 | 17 | Serial2 — **com resistor de 1kΩ em série** entre o TX2 e o RX do DFPlayer |
| Botão Play/Pause | 18 | `INPUT_PULLUP` |
| Botão Next | 5 | `INPUT_PULLUP` |
| Botão Prev | 19 | `INPUT_PULLUP` |
| Botão Modo | 21 | `INPUT_PULLUP` (clique único / duplo clique) |
| Potenciômetro de volume | 34 | Entrada analógica (ADC) |

> Os botões usam `INPUT_PULLUP`, ou seja, ficam em nível alto em repouso e vão pra `LOW` quando pressionados (um lado no GPIO, o outro no GND).

> **Sobre a ligação serial com o DFPlayer:** o **TX do DFPlayer vai direto no RX2 (GPIO 16) do ESP32** (essa direção pode ser ligada sem resistor, já que é o DFPlayer "falando" e o ESP32 só "ouvindo"). Já o **TX2 (GPIO 17) do ESP32, que vai no RX do DFPlayer, passa por um resistor de 1kΩ em série** antes de chegar no módulo. Esse resistor limita a corrente que entra no pino RX do DFPlayer, protegendo tanto o módulo quanto o próprio GPIO do ESP32 contra picos/curtos acidentais nessa linha — é uma prática comum (e recomendada pelo próprio fabricante/comunidade) ao ligar o DFPlayer Mini em microcontroladores, já que o pino RX dele é relativamente sensível a ruído e sobrecorrente vindos da linha serial.

---

## Fotos do circuito

O projeto passou por duas versões: primeiro foi montado em protoboard só pra testar e validar o circuito, e depois migrou pra uma placa em PCB.

**I) Protótipo em protoboard (versão real, em uso):**

| 📸 Circuito Montado |
| :---: |
| <img src="https://github.com/MathFerreiraDev/MP3-with-Wifi/blob/main/assets/ft1.jpg?raw=true" width="500" /> |
| <img src="https://github.com/MathFerreiraDev/MP3-with-Wifi/blob/main/assets/ft2.jpg?raw=true" width="500" /> |
| **Vídeo Explicativo** |
| <a href="https://www.youtube.com/watch?v=WWIJ_E87mHc"> <img width="1920" height="1080" alt="thumb" src="https://github.com/MathFerreiraDev/MP3-with-Wifi/blob/main/assets/thumb.png?raw=true" /> </a> |

**II) Esquemático:**

<img src="https://github.com/MathFerreiraDev/MP3-with-Wifi/blob/main/assets/esquematico.jpg?raw=true" width="900" />

**III) Versão em PCB:**

<img src="https://github.com/MathFerreiraDev/MP3-with-Wifi/blob/main/assets/pcb.jpg?raw=true" width="800" />

> A primeira versão do projeto, que é a que aparece na foto acima em protoboard, foi montada assim por motivo de prototipação — mais fácil de testar, ajustar fiação e corrigir algo sem precisar refazer solda. A versão em PCB vem como uma versão definitiva, já com o circuito validado.

---

## Funcionalidades

### 🎵 Controle de faixa

Os botões de play/pause, next e prev só funcionam quando a tela do player está aberta (se você tiver no menu WiFi ou na tela de "sem SD", eles não fazem nada — foi de propósito, pra não sair trocando de música escondido enquanto mexe no WiFi). Isso é resolvido checando o estado da tela atual antes de executar qualquer ação do botão.

- **Play/Pause:** pausa ou retoma a faixa atual, e o ícone na tela muda (barrinhas de pause ou o triângulo de play).
- **Next/Prev:** troca de faixa na hora. Se chegar na última faixa e apertar next, volta pra primeira (e vice-versa no prev) — wrap-around simples com base no total de faixas lido do cartão.
- Quando uma música termina sozinha, ele já avança pra próxima automaticamente (detectando o aviso `DFPlayerPlayFinished` que o módulo manda pela serial) e também volta pro início da lista se acabar a última.
- O nome da faixa aparece em cima do "disco" e, se for muito grande pra caber na tela, ele fica rolando tipo letreiro (efeito marquee), avançando um caractere a cada ~300ms até dar a volta completa no nome e recomeçar.
- O **volume** é lido do potenciômetro e convertido com uma curva não linear (não é uma regra de três direta): isso faz o giro do potenciômetro responder de um jeito mais parecido com o ouvido humano, em vez de pular de "quase mudo" pra "no talo" rapidinho na primeira metade do curso.

<img src="https://github.com/MathFerreiraDev/MP3-with-Wifi/blob/main/assets/display_menu.jpg?raw=true" width="400" />

---

### 📶 Modo WiFi

Um clique simples no botão de modo abre o modo WiFi: o ESP32 sobe uma rede própria chamada **`MP3-Player`** (aberta, sem senha) e vira um mini servidor web. Ao conectar no celular/PC, abre uma página (tipo um portal cativo, então nem precisa digitar IP) onde dá pra:

- **Acertar o relógio** exibido no display.
- **Renomear as faixas** — dá o nome que quiser pra cada número de música, e esse nome fica salvo direto na memória do ESP32, aparecendo depois no letreiro da tela do player.

O "portal cativo" funciona com um `DNSServer` interno que responde qualquer domínio com o IP do próprio ESP32, e o servidor web redireciona qualquer rota desconhecida pra página principal — por isso o celular geralmente já abre a página sozinho ao conectar, sem precisar digitar nada.

Enquanto esse modo tá ativo, o display mostra uma tela avisando que o WiFi está no ar e o nome da rede pra conectar. Pra sair do modo WiFi é só clicar de novo no botão de modo, que ele desliga o WiFi e volta pra tela do player (ou pra tela de "sem SD", se for o caso).

<img src="https://github.com/MathFerreiraDev/MP3-with-Wifi/blob/main/assets/display_wifi.jpg?raw=true" width="400" />

---

### 🔋 Modo "pseudo desligamento" (baixo consumo)

Dando um **duplo clique** no botão de modo, o player entra em modo de baixo consumo: a tela apaga (comando `SSD1306_DISPLAYOFF`, direto no controlador do OLED), a música pausa, o WiFi desliga (se estiver ligado) e o ESP32 passa a fazer bem pouca coisa — só fica de olho no relógio, salvando a hora certa de tempos em tempos (a cada 1 minuto) na memória interna.

Um novo clique (simples) no botão de modo liga tudo de volta: a tela acende, a música volta a tocar de onde parou e o relógio segue certinho.

Vale lembrar que isso não é um desligamento de verdade — o ESP32 continua ligado e consumindo (bem pouco) através da bateria 18650, gerenciada pelo módulo **TP4056**. A ideia aqui é só economizar energia sem perder a configuração de hora nem precisar reiniciar o player toda vez.

---

### 🛑 Cartão SD não encontrado

Se o DFPlayer não conseguir achar o cartão SD (ou ele não estiver inserido), o display mostra uma tela de aviso avisando que nenhum cartão foi detectado. A detecção acontece já na inicialização: o ESP32 tenta se conectar ao DFPlayer em até 3 tentativas (espaçadas em 3 segundos cada, pra não travar módulos clone), e depois pergunta ao próprio chip quantas faixas de áudio ele encontrou no cartão inteiro — se a contagem vier zerada, ele tenta ler de novo mais algumas vezes antes de desistir.

Nesse estado, os botões de play/next/prev ficam inativos, e o ESP32 fica tentando detectar o cartão de novo sozinho a cada 10 segundos, sem precisar reiniciar nada — assim que o SD aparece, ele já volta direto pra tela do player e começa a tocar a partir da primeira faixa.

<img src="https://github.com/MathFerreiraDev/MP3-with-Wifi/blob/main/assets/display_cartao.jpg?raw=true" width="400" />

---

## Detalhes de implementação (pra quem for mexer no código)

- Os botões usam **debounce por borda** (detecta a transição de estado, com uma janela de 30ms de estabilização), não por nível — assim segurar o botão pressionado não fica repetindo a ação sozinho.
- O botão de modo espera uma janelinha de **~350ms** pra saber se foi clique único ou duplo clique antes de decidir o que fazer.
- O relógio é só um contador por software (baseado no `millis()`), então ele precisa ser acertado pelo menos uma vez pelo modo WiFi; a hora (epoch) fica salva na memória interna (**NVS**, via biblioteca `Preferences`) periodicamente — a cada 1 minuto, ao entrar em baixo consumo e ao ser ajustada pela página web — pra não se perder quando desliga.
- Os nomes das faixas ficam guardados num arquivinho **JSON** (`/tracks.json`) na memória flash do próprio ESP32 (**LittleFS**), indexado pelo número da faixa, com até 31 caracteres por nome.
- A contagem total de faixas não é declarada manualmente: o firmware manda um comando ao DFPlayer que devolve quantos arquivos de áudio existem no cartão inteiro (incluindo os dentro de `SD:/mp3/`), e usa esse número pro wrap-around do next/prev e pra montar a lista de renomeação na página web.
- A leitura do potenciômetro só acontece enquanto a tela do player está ativa, e só atualiza o volume do DFPlayer quando a variação lida é grande o suficiente — evita ficar mandando comando pra ele o tempo todo à toa.
- A animação de "equalizador" e o letreiro do nome da faixa também só rodam/atualizam o display na tela do player (com exceção do letreiro, que continua rolando mesmo com a música pausada).

---

## Componentes usados

| Componente | Modelo | Função |
|---|---|---|
| Microcontrolador | ESP32 DevKit V1 | Controla o player, o display, os botões e o WiFi |
| Player de áudio | DFPlayer Mini | Lê o cartão SD e toca o MP3 |
| Resistor | 1 kΩ | Em série entre o TX2 do ESP32 e o RX do DFPlayer, limitando a corrente nessa linha e protegendo o pino contra picos/curtos |
| Display | OLED 0.96" SSD1306 (I2C) | Mostra a tela do player/WiFi/aviso de SD |
| Botões | 4x push-button | Play/pause, next, prev e modo |
| Potenciômetro | 10 kΩ | Ajuste de volume |
| Saída de áudio | Jack P2 fêmea | Conecta fone ou caixinha |
| Bateria | 18650 | Alimentação portátil |
| Carregador | Módulo TP4056 | Carrega a bateria via USB e alimenta o circuito |

### Bibliotecas (firmware)

- `Adafruit SSD1306` + `Adafruit GFX Library` — display OLED
- `DFRobotDFPlayerMini` — controle do módulo de áudio
- `WiFi` + `WebServer` + `DNSServer` (core ESP32) — modo WiFi / portal cativo
- `Preferences` — persistência do relógio na NVS
- `LittleFS` (já incluso no core ESP32 ≥ 2.x) — armazenamento dos nomes de faixa
- `ArduinoJson` (v6.x) — leitura/escrita do JSON de nomes de faixa

---

## Créditos

Projeto feito no ICMC - USP por Matheus H. Ferreira.
