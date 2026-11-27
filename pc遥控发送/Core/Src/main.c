/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
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

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include <string.h>
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
UART_HandleTypeDef huart1;
UART_HandleTypeDef huart2;

/* USER CODE BEGIN PV */
#define PC_RX_RING_SIZE 128U

static volatile uint8_t pc_rx_ring[PC_RX_RING_SIZE];
static volatile uint16_t pc_rx_head;
static volatile uint16_t pc_rx_tail;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_USART1_UART_Init(void);
static void MX_USART2_UART_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

#define MOTOR_COUNT         4U
#define RF_FRAME_LENGTH     7U
#define RF_CHANNEL_SLOT_MS  50U
#define PC_LINK_TIMEOUT_MS  1000U
#define PC_STATUS_MS        1000U
#define PC_LINE_SIZE        48U
#define THROTTLE_DEFAULT    60U
#define THROTTLE_MAX        100U

#define RF_KEY_STOP         0x00U
#define RF_KEY_FORWARD_BASE 0x10U
#define RF_KEY_REVERSE_BASE 0x80U
#define RF_ADDR_PROTOCOL    0xA5U

typedef enum
{
  REMOTE_STOP = 0,
  REMOTE_FORWARD,
  REMOTE_REVERSE
} RemoteDirection;

typedef struct
{
  RemoteDirection direction;
  uint8_t throttle;
} RemoteMotor;

static RemoteMotor motors[MOTOR_COUNT];
static uint8_t rf_channel_index;
static uint8_t pc_connected;
static uint32_t pc_last_rx_tick;
static uint32_t rf_last_tx_tick;
static uint32_t pc_last_status_tick;
static char pc_line[PC_LINE_SIZE];
static uint8_t pc_line_length;

static char Direction_Char(RemoteDirection direction)
{
  if (direction == REMOTE_FORWARD)
  {
    return 'F';
  }
  if (direction == REMOTE_REVERSE)
  {
    return 'R';
  }
  return 'S';
}

static uint8_t Motor_Key(uint8_t index)
{
  if ((motors[index].direction == REMOTE_FORWARD) &&
      (motors[index].throttle > 0U))
  {
    return (uint8_t)(RF_KEY_FORWARD_BASE + motors[index].throttle - 1U);
  }
  if ((motors[index].direction == REMOTE_REVERSE) &&
      (motors[index].throttle > 0U))
  {
    return (uint8_t)(RF_KEY_REVERSE_BASE + motors[index].throttle - 1U);
  }
  return RF_KEY_STOP;
}

static void Motor_Set(uint8_t index, RemoteDirection direction, uint8_t throttle)
{
  if (index >= MOTOR_COUNT)
  {
    return;
  }
  motors[index].throttle = throttle;
  motors[index].direction =
      (throttle == 0U) ? REMOTE_STOP : direction;
}

static void Motor_StopAll(void)
{
  uint8_t index;

  for (index = 0U; index < MOTOR_COUNT; index++)
  {
    motors[index].direction = REMOTE_STOP;
  }
}

static void PC_Write(const char *text)
{
  HAL_UART_Transmit(&huart2, (uint8_t *)text, (uint16_t)strlen(text), 100U);
}

static void PC_SendStatus(void)
{
  char text[128];
  uint32_t uptime = HAL_GetTick();

  (void)snprintf(text, sizeof(text),
                 "STATUS,M1=%c%u,M2=%c%u,M3=%c%u,M4=%c%u,pc=%u,uptime=%lu\r\n",
                 Direction_Char(motors[0].direction), (unsigned int)motors[0].throttle,
                 Direction_Char(motors[1].direction), (unsigned int)motors[1].throttle,
                 Direction_Char(motors[2].direction), (unsigned int)motors[2].throttle,
                 Direction_Char(motors[3].direction), (unsigned int)motors[3].throttle,
                 (unsigned int)pc_connected,
                 (unsigned long)uptime);
  PC_Write(text);
}

static void PC_SendAck(void)
{
  PC_SendStatus();
}

static uint8_t PC_ParseThrottle(const char *text, uint8_t *value)
{
  uint16_t parsed = 0U;
  uint8_t digits = 0U;

  while ((*text == ' ') || (*text == '\t'))
  {
    text++;
  }
  while ((*text >= '0') && (*text <= '9'))
  {
    parsed = (uint16_t)((parsed * 10U) + (uint16_t)(*text - '0'));
    digits++;
    text++;
    if (parsed > THROTTLE_MAX)
    {
      return 0U;
    }
  }
  while ((*text == ' ') || (*text == '\t'))
  {
    text++;
  }
  if ((digits == 0U) || (*text != '\0'))
  {
    return 0U;
  }

  *value = (uint8_t)parsed;
  return 1U;
}

static uint8_t PC_ParseDirection(char ch, RemoteDirection *direction)
{
  if (ch == 'F')
  {
    *direction = REMOTE_FORWARD;
    return 1U;
  }
  if (ch == 'R')
  {
    *direction = REMOTE_REVERSE;
    return 1U;
  }
  if (ch == 'S')
  {
    *direction = REMOTE_STOP;
    return 1U;
  }
  return 0U;
}

static uint8_t PC_ParseChannel(char ch, uint8_t *index)
{
  if ((ch < '1') || (ch > '4'))
  {
    return 0U;
  }
  *index = (uint8_t)(ch - '1');
  return 1U;
}

static void PC_SetAll(RemoteDirection direction)
{
  uint8_t index;

  for (index = 0U; index < MOTOR_COUNT; index++)
  {
    Motor_Set(index, direction, motors[index].throttle);
  }
}

static void PC_SetAllThrottle(uint8_t throttle)
{
  uint8_t index;

  for (index = 0U; index < MOTOR_COUNT; index++)
  {
    Motor_Set(index, motors[index].direction, throttle);
  }
}

static void PC_ExecuteLine(char *line)
{
  uint8_t index;
  uint8_t throttle;
  RemoteDirection direction;

  if ((strcmp(line, "S") == 0) || (strcmp(line, "STOP") == 0))
  {
    Motor_StopAll();
    PC_SendAck();
  }
  else if ((strcmp(line, "F") == 0) || (strcmp(line, "FORWARD") == 0))
  {
    PC_SetAll(REMOTE_FORWARD);
    PC_SendAck();
  }
  else if ((strcmp(line, "R") == 0) || (strcmp(line, "REVERSE") == 0))
  {
    PC_SetAll(REMOTE_REVERSE);
    PC_SendAck();
  }
  else if ((strncmp(line, "M,", 2U) == 0) &&
           (strlen(line) >= 7U) &&
           (line[3] == ',') &&
           (line[5] == ','))
  {
    if (PC_ParseChannel(line[2], &index) == 0U)
    {
      PC_Write("ERR,CHANNEL\r\n");
    }
    else if (PC_ParseDirection(line[4], &direction) == 0U)
    {
      PC_Write("ERR,DIRECTION\r\n");
    }
    else if (PC_ParseThrottle(&line[6], &throttle) == 0U)
    {
      PC_Write("ERR,THROTTLE\r\n");
    }
    else
    {
      Motor_Set(index, direction, throttle);
      PC_SendAck();
    }
  }
  else if ((strncmp(line, "V,", 2U) == 0) &&
           (strlen(line) >= 6U) &&
           (line[3] == ','))
  {
    if (PC_ParseChannel(line[2], &index) == 0U)
    {
      PC_Write("ERR,CHANNEL\r\n");
    }
    else if (PC_ParseThrottle(&line[4], &throttle) == 0U)
    {
      PC_Write("ERR,THROTTLE\r\n");
    }
    else
    {
      Motor_Set(index, motors[index].direction, throttle);
      PC_SendAck();
    }
  }
  else if (strncmp(line, "V,", 2U) == 0)
  {
    if (PC_ParseThrottle(&line[2], &throttle) == 0U)
    {
      PC_Write("ERR,THROTTLE\r\n");
    }
    else
    {
      PC_SetAllThrottle(throttle);
      PC_SendAck();
    }
  }
  else if ((strncmp(line, "C,", 2U) == 0) &&
           (strlen(line) >= 5U) &&
           (line[3] == ','))
  {
    if (PC_ParseDirection(line[2], &direction) == 0U)
    {
      PC_Write("ERR,DIRECTION\r\n");
    }
    else if (PC_ParseThrottle(&line[4], &throttle) == 0U)
    {
      PC_Write("ERR,THROTTLE\r\n");
    }
    else
    {
      for (index = 0U; index < MOTOR_COUNT; index++)
      {
        Motor_Set(index, direction, throttle);
      }
      PC_SendAck();
    }
  }
  else if (strcmp(line, "PING") == 0)
  {
    char text[40];
    (void)snprintf(text, sizeof(text), "PONG,%lu\r\n", (unsigned long)HAL_GetTick());
    PC_Write(text);
  }
  else if ((strcmp(line, "STATUS") == 0) || (strcmp(line, "STATUS?") == 0))
  {
    PC_SendStatus();
  }
  else if (strcmp(line, "H") != 0)
  {
    PC_Write("ERR,COMMAND\r\n");
  }
}

static void PC_HandleLine(void)
{
  char normalized[PC_LINE_SIZE];
  uint8_t i;

  while ((pc_line_length > 0U) &&
         ((pc_line[pc_line_length - 1U] == ' ') ||
          (pc_line[pc_line_length - 1U] == '\t')))
  {
    pc_line_length--;
  }
  pc_line[pc_line_length] = '\0';
  pc_last_rx_tick = HAL_GetTick();
  pc_connected = 1U;
  if (pc_line_length == 0U)
  {
    return;
  }

  for (i = 0U; (i < pc_line_length) && (i < (PC_LINE_SIZE - 1U)); i++)
  {
    char ch = pc_line[i];
    if ((ch >= 'a') && (ch <= 'z'))
    {
      ch = (char)(ch - ('a' - 'A'));
    }
    normalized[i] = ch;
  }
  normalized[i] = '\0';
  PC_ExecuteLine(normalized);
  pc_line_length = 0U;
}

static void PC_ProcessRxChar(uint8_t ch)
{
  if ((ch == '\r') || (ch == '\n'))
  {
    PC_HandleLine();
  }
  else if ((ch == '\b') || (ch == 0x7FU))
  {
    if (pc_line_length > 0U)
    {
      pc_line_length--;
    }
  }
  else if ((ch >= 0x20U) && (ch <= 0x7EU))
  {
    if (pc_line_length < (PC_LINE_SIZE - 1U))
    {
      pc_line[pc_line_length] = (char)ch;
      pc_line_length++;
    }
    else
    {
      pc_line_length = 0U;
      PC_Write("ERR,LINE_TOO_LONG\r\n");
    }
  }
}

static uint8_t PC_RingRead(uint8_t *ch)
{
  if (pc_rx_tail == pc_rx_head)
  {
    return 0U;
  }
  *ch = pc_rx_ring[pc_rx_tail];
  pc_rx_tail = (uint16_t)((pc_rx_tail + 1U) % PC_RX_RING_SIZE);
  return 1U;
}

void USART2_IRQHandler(void)
{
  uint32_t status = USART2->SR;

  if ((status & (USART_SR_RXNE | USART_SR_ORE)) != 0U)
  {
    uint8_t ch = (uint8_t)USART2->DR;
    uint16_t next;

    if ((status & USART_SR_RXNE) != 0U)
    {
      next = (uint16_t)((pc_rx_head + 1U) % PC_RX_RING_SIZE);
      if (next != pc_rx_tail)
      {
        pc_rx_ring[pc_rx_head] = ch;
        pc_rx_head = next;
      }
    }
  }
}

static void RF_SendNextMotor(void)
{
  uint8_t frame[RF_FRAME_LENGTH] = {0xFD, 0x01, RF_ADDR_PROTOCOL, 0x01, 0x00, 0x40, 0xDF};

  frame[3] = (uint8_t)(rf_channel_index + 1U);
  frame[4] = Motor_Key(rf_channel_index);
  (void)HAL_UART_Transmit(&huart1, frame, RF_FRAME_LENGTH, 100U);
  rf_last_tx_tick = HAL_GetTick();
  rf_channel_index = (uint8_t)((rf_channel_index + 1U) % MOTOR_COUNT);
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

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_USART1_UART_Init();
  MX_USART2_UART_Init();
  /* USER CODE BEGIN 2 */

  {
    uint8_t index;
    for (index = 0U; index < MOTOR_COUNT; index++)
    {
      motors[index].direction = REMOTE_STOP;
      motors[index].throttle = THROTTLE_DEFAULT;
    }
  }
  rf_channel_index = 0U;
  pc_connected = 0U;
  pc_line_length = 0U;
  pc_rx_head = 0U;
  pc_rx_tail = 0U;
  HAL_Delay(500U);
  rf_last_tx_tick = HAL_GetTick() - RF_CHANNEL_SLOT_MS;
  pc_last_status_tick = HAL_GetTick();
  PC_Write("BOOT,auvtxt,2\r\n");

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    uint8_t ch;
    uint32_t now = HAL_GetTick();

    while (PC_RingRead(&ch) != 0U)
    {
      PC_ProcessRxChar(ch);
    }

    if ((pc_connected != 0U) &&
        ((now - pc_last_rx_tick) > PC_LINK_TIMEOUT_MS))
    {
      pc_connected = 0U;
      Motor_StopAll();
      PC_Write("ERR,PC_TIMEOUT\r\n");
    }

    if ((now - rf_last_tx_tick) >= RF_CHANNEL_SLOT_MS)
    {
      RF_SendNextMotor();
    }

    if ((pc_connected != 0U) &&
        ((now - pc_last_status_tick) >= PC_STATUS_MS))
    {
      PC_SendStatus();
      pc_last_status_tick = now;
    }
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
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
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

/**
  * @brief USART1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART1_UART_Init(void)
{

  /* USER CODE BEGIN USART1_Init 0 */

  /* USER CODE END USART1_Init 0 */

  /* USER CODE BEGIN USART1_Init 1 */

  /* USER CODE END USART1_Init 1 */
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 9600;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART1_Init 2 */

  /* USER CODE END USART1_Init 2 */

}

/**
  * @brief USART2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART2_UART_Init(void)
{

  /* USER CODE BEGIN USART2_Init 0 */

  /* USER CODE END USART2_Init 0 */

  /* USER CODE BEGIN USART2_Init 1 */

  /* USER CODE END USART2_Init 1 */
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 115200;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART2_Init 2 */
  __HAL_UART_CLEAR_OREFLAG(&huart2);
  __HAL_UART_ENABLE_IT(&huart2, UART_IT_RXNE);
  /* USER CODE END USART2_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

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
