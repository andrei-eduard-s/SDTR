/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "cmsis_os.h" // Folosim RTOS (FreeRTOS prin CMSIS-RTOS2 API)
#include "tim.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include <string.h>
#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "task.h"   // pentru vTaskGetRunTimeStats()

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

static char txBuf[128];
static volatile uint8_t uartBusy = 0;
static uint8_t firstE2E = 1;

// Queue pentru datele brute de la senzor
osMessageQueueId_t qSensorHandle; // Sensor -> Control

// Queue pentru datele procesate de control (pentru debug)
osMessageQueueId_t qTlmHandle; // Control -> Debug

// Mutex pentru UART (ca sa nu scriem 2 task-uri simultan pe acelasi port)
osMutexId_t uartMutexHandle;

// Structura pentru pachetele transmise intre task-uri
typedef struct {
    uint8_t temp;
    uint8_t hum;
    uint8_t duty;
    uint32_t t0_cycles;   // timestamp la citirea senzorului
} SensorData_t;

// Duty cycle global (0..100) -  Variabila globala pentru duty-cycle (modificata de task Control)
static uint8_t duty = 0;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
void MX_FREERTOS_Init(void);
/* USER CODE BEGIN PFP */
void StartTask_Sensor(void *argument);
void StartTask_Control(void *argument);
void StartTask_Debug(void *argument);
void DHT11_Read(uint8_t *temp, uint8_t *hum);

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
// --- DHT11 driver ---

#define DHT_PORT GPIOB
#define DHT_PIN  GPIO_PIN_5

static void delay_us(uint16_t us)
{
    uint32_t start = DWT->CYCCNT;
    uint32_t cycles = us * (SystemCoreClock/1000000);
    while((DWT->CYCCNT - start) < cycles);
}

static void DHT_set_output(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin = DHT_PIN;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(DHT_PORT, &GPIO_InitStruct);
}

static void DHT_set_input(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin = DHT_PIN;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(DHT_PORT, &GPIO_InitStruct);
}

void DHT11_Read(uint8_t *temp, uint8_t *hum)
{
    uint8_t bits[5] = {0};

    // Start signal
    DHT_set_output();
    HAL_GPIO_WritePin(DHT_PORT, DHT_PIN, GPIO_PIN_RESET);
    osDelay(20); // 18 ms
    HAL_GPIO_WritePin(DHT_PORT, DHT_PIN, GPIO_PIN_SET);
    delay_us(30);
    DHT_set_input();

    // Wait for DHT response
    uint32_t t=0;
    while(HAL_GPIO_ReadPin(DHT_PORT,DHT_PIN)==GPIO_PIN_SET){ if(++t>10000) return; }
    while(HAL_GPIO_ReadPin(DHT_PORT,DHT_PIN)==GPIO_PIN_RESET);
    while(HAL_GPIO_ReadPin(DHT_PORT,DHT_PIN)==GPIO_PIN_SET);

    // Read 40 bits
    for(int i=0;i<40;i++){
        while(HAL_GPIO_ReadPin(DHT_PORT,DHT_PIN)==GPIO_PIN_RESET);
        uint32_t start = DWT->CYCCNT;
        while(HAL_GPIO_ReadPin(DHT_PORT,DHT_PIN)==GPIO_PIN_SET);
        uint32_t width = (DWT->CYCCNT - start) / (SystemCoreClock/1000000);
        if(width > 40) bits[i/8] |= (1 << (7-(i%8)));
    }

    // Verify checksum
    if(bits[4] == (uint8_t)(bits[0]+bits[1]+bits[2]+bits[3])){
        *hum = bits[0];
        *temp = bits[2];
    }
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  // Enable DWT CYCCNT so delay_us() works
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  // Initializare hardware pentru interactiune cu mediul real
  MX_GPIO_Init();
  MX_USART2_UART_Init(); // USB debug
  MX_USART3_UART_Init(); // Bluetooth HC-05 = protocol comunicatie
  MX_TIM3_Init();
  HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_1); // Pornim PWM (iesire timp real)

  // init FreeRTOS
  osKernelInitialize();

  // configureaza timerul de runtime stats
  portCONFIGURE_TIMER_FOR_RUN_TIME_STATS();

  // Cream cozi pentru sincronizare intre procese
  qSensorHandle = osMessageQueueNew(5, sizeof(SensorData_t), NULL);
  qTlmHandle    = osMessageQueueNew(5, sizeof(SensorData_t), NULL);

  // Cream mutex pentru concurenta pe UART
  uartMutexHandle = osMutexNew(NULL);

  // Atribuim prioritati pentru cele 3 task-uri
  const osThreadAttr_t attrSensor  = { .name = "Sensor",  .priority = osPriorityNormal,      .stack_size = 512 };
  const osThreadAttr_t attrControl = { .name = "Control", .priority = osPriorityAboveNormal, .stack_size = 512 };
  const osThreadAttr_t attrDebug   = { .name = "Debug",   .priority = osPriorityBelowNormal, .stack_size = 512 };

  // Cream task-urile / procesele
  osThreadNew(StartTask_Sensor,  NULL, &attrSensor);
  osThreadNew(StartTask_Control, NULL, &attrControl);
  osThreadNew(StartTask_Debug,   NULL, &attrDebug);

  /* USER CODE BEGIN 2 */
  // Protocol comunicatie -> Bluetooth
  const char *boot = "BOOT via HC-05\r\n";
  HAL_UART_Transmit(&huart3, (uint8_t*)boot, strlen(boot), HAL_MAX_DELAY);

  /* USER CODE END 2 */

  /* Init scheduler */

  /* Start scheduler */
  // Pornim planificatorul RTOS
  osKernelStart();

  /* We should never get here as control is now taken by the scheduler */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */

  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI_DIV2;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL16;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart) {
    if (huart->Instance == USART3) {
        uartBusy = 0; // transmisia pe Bluetooth s-a terminat
    }
}

// Task 1 (Normal): citeste DHT11 si pune date brute in qSensor
void StartTask_Sensor(void *argument)
{
  for(;;)
  {
    uint8_t t=0,h=0;

    uint32_t t0 = DWT->CYCCNT;   // ia timestamp inainte de citire
    DHT11_Read(&t,&h); // citire senzor temperatura + umiditate

    SensorData_t pkt = { t, h, 0, t0 }; // stocheaza timestamp-ul real

    osMessageQueuePut(qSensorHandle, &pkt, 0, 0); // pune in coada (non-blocant)

    osDelay(1000); // DHT11 citit o data pe secunda
  }
}

// Task 2 (AboveNormal): proceseaza datele si ajusteaza PWM; publica telemetria
void StartTask_Control(void *argument)
{
  SensorData_t in, out;
  for(;;)
  {
    if (osMessageQueueGet(qSensorHandle, &in, NULL, osWaitForever) == osOK)
    {
      // Algoritm simplu de control pe trepte
      if      (in.temp <= 22) duty = 20;
      else if (in.temp <= 24) duty = 30;
      else if (in.temp <= 26) duty = 40;
      else if (in.temp <= 28) duty = 60;
      else if (in.temp <= 30) duty = 80;
      else                    duty = 90;

      // Aplica PWM = interactiune in timp real cu mediul (ventilator)
      __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_1,
         (duty * (__HAL_TIM_GET_AUTORELOAD(&htim3)+1))/100);

      // Pregatim pachetul pentru transmisie prin Bluetooth
      out = in;            // copiaza TOT, inclusiv t0_cycles
      out.duty = duty;     // actualizeaza doar duty
      osMessageQueuePut(qTlmHandle, &out, 0, 0); // Control -> Debug

    }
  }
}

// Task 3 (BelowNormal): consumator unic de telemetrie -> afiseaza pe UART USB/BT
void StartTask_Debug(void *argument)
{
  SensorData_t s;

  for(;;)
  {
    if (osMessageQueueGet(qTlmHandle, &s, NULL, osWaitForever) == osOK)
    {
      osMutexAcquire(uartMutexHandle, osWaitForever);

      if (firstE2E) {
          // doar la prima masurare calculam si E2E
          uint32_t now = DWT->CYCCNT;
          uint32_t dt_us = (now - s.t0_cycles) / (SystemCoreClock / 1000000U);

          sprintf(txBuf, "Temp=%d C, Hum=%d %%, Duty=%d %%, E2E=%lu us\r\n",
                  s.temp, s.hum, s.duty, (unsigned long)dt_us);

          firstE2E = 0; // dupa prima data nu mai afisam E2E
      } else {
          // restul timpului doar telemetrie
          sprintf(txBuf, "Temp=%d C, Hum=%d %%, Duty=%d %%\r\n",
                  s.temp, s.hum, s.duty);
      }

      // Transmitere blocking simpla
      HAL_UART_Transmit(&huart3, (uint8_t*)txBuf, strlen(txBuf), HAL_MAX_DELAY);

      osMutexRelease(uartMutexHandle);
    }
  }
}

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
