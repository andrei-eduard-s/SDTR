# SDTR
# Sistem de ventilație adaptiv

Acest proiect constă în dezvoltarea unui sistem adaptiv de ventilație pentru medii industriale sau locuințe personale folosind placa de dezvoltare STM32 NUCLEO-F103RB cu microcontroler Cortex-M3 și debugger integrat. Sistemul ajustează automat fluxul de aer în funcție de temperatura și umiditatea. Funcționarea în timp real asigură un răspuns rapid la orice schimbare pentru a menține condiții optime, iar utilizatorii pot monitoriza și modifica setările printr-o interfață Bluetooth.

## Cerințe și implementare

✔ **RTOS utilizat**: FreeRTOS prin CMSIS-RTOS2 API  
```c
#include "cmsis_os.h" // Folosim RTOS (FreeRTOS prin CMSIS-RTOS2 API)
```

✔ **Cel puțin 2 procese**: proiectul are 3 task-uri independente:
- `StartTask_Sensor` – citire senzor DHT11
- `StartTask_Control` – procesare date și control ventilator
- `StartTask_Debug` – transmitere telemetrie prin Bluetooth

✔ **Un proces de timp real cu interacțiune cu mediul exterior**:  
`StartTask_Control` citește datele și modifică PWM-ul ventilatorului:  
```c
__HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_1,
   (duty * (__HAL_TIM_GET_AUTORELOAD(&htim3)+1))/100);
```

✔ **Un proces de comunicație la distanță**:  
`StartTask_Debug` transmite periodic date prin UART3 → modul Bluetooth HC-05.  
```c
sprintf(txBuf, "Temp=%d C, Hum=%d %%, Duty=%d %%\r\n",
        s.temp, s.hum, s.duty);
HAL_UART_Transmit(&huart3, (uint8_t*)txBuf, strlen(txBuf), HAL_MAX_DELAY);
```

✔ **Procese independente, sincronizate prin cozi de mesaje**:  
- `qSensorHandle` leagă **Sensor → Control**  
- `qTlmHandle` leagă **Control → Debug**  
```c
qSensorHandle = osMessageQueueNew(5, sizeof(SensorData_t), NULL);
qTlmHandle    = osMessageQueueNew(5, sizeof(SensorData_t), NULL);
```

✔ **Tratamentul concurenței**: mutex pe UART pentru a preveni accesul simultan.  
```c
uartMutexHandle = osMutexNew(NULL);
osMutexAcquire(uartMutexHandle, osWaitForever);
// ...
osMutexRelease(uartMutexHandle);
```

---

## Specificații

✔ **Microcontroller**: STM32F103RB (ARM Cortex-M3, placă Nucleo).  
✔ **Resurse hardware integrate**: Timer3 PWM, GPIO pentru DHT11, UART3 pentru HC-05.  
✔ **Interfață de intrare**: senzor DHT11 (temperatură și umiditate).  
✔ **Interfață de ieșire**: ventilator controlat PWM și telemetrie transmisă Bluetooth.  
✔ **Protocol de comunicație**: UART → Bluetooth (HC-05).  
✔ **Răspuns în timp real**: control ventilator în funcție de temperatură, raportare prin Bluetooth.

---

## Structura software

### Task 1 – Citirea senzorului (`StartTask_Sensor`)
- Rulează la prioritate normală.
- Citește temperatura și umiditatea de la DHT11.
- Pune datele brute într-o coadă (`qSensorHandle`).

```c
void StartTask_Sensor(void *argument) {
  for(;;) {
    uint8_t t=0,h=0;
    DHT11_Read(&t,&h);
    SensorData_t pkt = { t, h, 0, DWT->CYCCNT };
    osMessageQueuePut(qSensorHandle, &pkt, 0, 0);
    osDelay(1000); // citire la 1s
  }
}
```

### Task 2 – Control ventilator (`StartTask_Control`)
- Rulează la prioritate AboveNormal.
- Preia datele din coada `qSensorHandle`.
- Decide duty-cycle PWM pe baza temperaturii.
- Pune pachetul procesat în coada `qTlmHandle`.

```c
if      (in.temp <= 22) duty = 20;
else if (in.temp <= 24) duty = 30;
else if (in.temp <= 26) duty = 40;
else if (in.temp <= 28) duty = 60;
else if (in.temp <= 30) duty = 80;
else                    duty = 90;
```

### Task 3 – Debug & Comunicație (`StartTask_Debug`)
- Rulează la prioritate BelowNormal.
- Preia date din `qTlmHandle`.
- Trimite pe UART3 către modulul Bluetooth HC-05.
- La prima execuție calculează și afișează **E2E latency** (end-to-end).

```c
if (firstE2E) {
  uint32_t now = DWT->CYCCNT;
  uint32_t dt_us = (now - s.t0_cycles) / (SystemCoreClock / 1000000U);
  sprintf(txBuf, "Temp=%d C, Hum=%d %%, Duty=%d %%, E2E=%lu us\r\n",
          s.temp, s.hum, s.duty, (unsigned long)dt_us);
  firstE2E = 0;
} else {
  sprintf(txBuf, "Temp=%d C, Hum=%d %%, Duty=%d %%\r\n",
          s.temp, s.hum, s.duty);
}
```

---

## Analiza performanțelor în timp real

La prima execuție, sistemul calculează durata **end-to-end (E2E)** dintre citirea senzorului DHT11, procesarea în task-ul de control și transmiterea mesajului pe Bluetooth.  

Rezultatul măsurat pe hardware:
```c
E2E ≈ 31574 µs (aprox. 31,5 ms)
```
Aceasta confirmă că întregul pipeline senzor → control → comunicație respectă cerințele de timp real, oferind un răspuns sub 50 ms.

## Concluzie

Acest proiect: 
- folosește **RTOS** pentru multitasking,  
- are **procese independente** (Sensor, Control, Debug),  
- folosește **cozi și mutex** pentru sincronizare,  
- interacționează în timp real cu mediul (ventilator PWM + senzor DHT11),  
- transmite date printr-un **protocol wireless (Bluetooth)**,  
- demonstrează un **pipeline end-to-end** de achiziție și reacție în timp real.
