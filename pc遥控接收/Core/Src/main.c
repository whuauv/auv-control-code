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
TIM_HandleTypeDef htim2;
TIM_HandleTypeDef htim4;

UART_HandleTypeDef huart1;

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_TIM2_Init(void);
static void MX_TIM4_Init(void);
static void MX_USART1_UART_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

#define RF_RX_MAX_LEN       7U
#define RF_FRAME_HEAD       0xFDU
#define RF_FRAME_TAIL       0xDFU
#define RF_ADDR_SHORT_INDEX 1U   /* 6 字节帧：FD addr2 addr1 key width DF */
#define RF_ADDR_LONG_INDEX  2U   /* 7 字节帧：FD time addr2 addr1 key width DF */
#define RF_CHANNEL_LONG_INDEX 3U
#define RF_KEY_SHORT_INDEX  3U   /* 6 字节帧：FD addr2 addr1 key width DF */
#define RF_KEY_LONG_INDEX   4U   /* 7 字节帧：FD time addr2 addr1 key width DF */
#define RF_ADDR_PROTOCOL    0xA5U
#define MOTOR_COUNT         4U
#define RF_KEY_FORWARD      0x01U
#define RF_KEY_STOP         0x02U
#define RF_KEY_REVERSE      0x03U
#define RF_KEY_STOP_EXT     0x00U
#define RF_KEY_FORWARD_BASE 0x10U
#define RF_KEY_REVERSE_BASE 0x80U
#define RF_KEY_FORWARD_MAX  (RF_KEY_FORWARD_BASE + 99U)  /* 0x73 */
#define RF_KEY_REVERSE_MAX  (RF_KEY_REVERSE_BASE + 99U)  /* 0xE3 */

#define MOTOR_PULSE_REVERSE 1200U
#define MOTOR_PULSE_STOP    1500U
#define MOTOR_PULSE_FORWARD 1800U
#define MOTOR_PULSE_SPAN    300U

static uint8_t rx_buffer[RF_RX_MAX_LEN];
static uint8_t rx_count;
static uint32_t last_frame_tick;

#define RF_LINK_TIMEOUT_MS 500U

/* M1/M2 使用 TIM4_CH3/CH4，M3/M4 使用 TIM2_CH1/CH2。 */
static void Motor_SetChannel(uint8_t index, uint16_t pulse)
{
  if (index == 0U)
  {
    __HAL_TIM_SET_COMPARE(&htim4, TIM_CHANNEL_3, pulse);
  }
  else if (index == 1U)
  {
    __HAL_TIM_SET_COMPARE(&htim4, TIM_CHANNEL_4, pulse);
  }
  else if (index == 2U)
  {
    __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, pulse);
  }
  else if (index == 3U)
  {
    __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_2, pulse);
  }
}

static void Motor_SetAll(uint16_t pulse)
{
  uint8_t index;

  for (index = 0U; index < MOTOR_COUNT; index++)
  {
    Motor_SetChannel(index, pulse);
  }
}

static uint16_t RF_PulseForKey(uint8_t key)
{
  uint8_t throttle;
  uint16_t pulse;

  if (key == RF_KEY_FORWARD)
  {
    return MOTOR_PULSE_FORWARD;
  }
  if (key == RF_KEY_REVERSE)
  {
    return MOTOR_PULSE_REVERSE;
  }
  if ((key == RF_KEY_STOP_EXT) || (key == RF_KEY_STOP))
  {
    return MOTOR_PULSE_STOP;
  }
  if ((key >= RF_KEY_FORWARD_BASE) && (key <= RF_KEY_FORWARD_MAX))
  {
    throttle = (uint8_t)(key - RF_KEY_FORWARD_BASE + 1U);
    pulse = (uint16_t)(MOTOR_PULSE_STOP +
                       ((uint32_t)throttle * MOTOR_PULSE_SPAN / 100U));
    return pulse;
  }
  if ((key >= RF_KEY_REVERSE_BASE) && (key <= RF_KEY_REVERSE_MAX))
  {
    throttle = (uint8_t)(key - RF_KEY_REVERSE_BASE + 1U);
    pulse = (uint16_t)(MOTOR_PULSE_STOP -
                       ((uint32_t)throttle * MOTOR_PULSE_SPAN / 100U));
    return pulse;
  }
  return MOTOR_PULSE_STOP;
}

static void RF_ApplyFrame(uint8_t addr2, uint8_t channel, uint8_t key)
{
  if ((addr2 == RF_ADDR_PROTOCOL) &&
      (channel >= 1U) &&
      (channel <= MOTOR_COUNT))
  {
    Motor_SetChannel((uint8_t)(channel - 1U), RF_PulseForKey(key));
  }
  else
  {
    Motor_SetAll(RF_PulseForKey(key));
  }
  last_frame_tick = HAL_GetTick();
}

static void RF_ParseByte(uint8_t ch)
{
  if (ch == RF_FRAME_HEAD)
  {
    rx_count = 0U;
    rx_buffer[rx_count] = ch;
    rx_count++;
    return;
  }

  if ((rx_count == 0U) || (rx_count >= RF_RX_MAX_LEN))
  {
    return;
  }

  rx_buffer[rx_count] = ch;
  rx_count++;

  /* YS-RF34T 默认输出 6 字节：FD addr2 addr1 key width DF */
  if ((rx_count == 6U) && (rx_buffer[5] == RF_FRAME_TAIL))
  {
    RF_ApplyFrame(rx_buffer[RF_ADDR_SHORT_INDEX],
                  rx_buffer[RF_ADDR_SHORT_INDEX + 1U],
                  rx_buffer[RF_KEY_SHORT_INDEX]);
    rx_count = 0U;
    return;
  }

  /* 兼容 7 字节格式：FD time addr2 addr1 key width DF */
  if (rx_count == RF_RX_MAX_LEN)
  {
    if (rx_buffer[RF_RX_MAX_LEN - 1U] == RF_FRAME_TAIL)
    {
      RF_ApplyFrame(rx_buffer[RF_ADDR_LONG_INDEX],
                    rx_buffer[RF_CHANNEL_LONG_INDEX],
                    rx_buffer[RF_KEY_LONG_INDEX]);
    }
    rx_count = 0U;
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

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_TIM2_Init();
  MX_TIM4_Init();
  MX_USART1_UART_Init();
  /* USER CODE BEGIN 2 */

  HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_1);
  HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_2);
  HAL_TIM_PWM_Start(&htim4, TIM_CHANNEL_3);
  HAL_TIM_PWM_Start(&htim4, TIM_CHANNEL_4);
  Motor_SetAll(MOTOR_PULSE_STOP);
  HAL_Delay(3000U);      /* 四路输出中位脉宽，等待两个电调完成解锁 */

  /* 解锁期间 USART 已经在接收，先清掉残留字节和溢出标志。 */
  while (__HAL_UART_GET_FLAG(&huart1, UART_FLAG_RXNE) != RESET)
  {
    (void)huart1.Instance->DR;
  }
  __HAL_UART_CLEAR_OREFLAG(&huart1);

  last_frame_tick = HAL_GetTick();

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    uint8_t ch;

    if (HAL_UART_Receive(&huart1, &ch, 1U, 20U) == HAL_OK)
    {
      RF_ParseByte(ch);
    }

    if ((HAL_GetTick() - last_frame_tick) > RF_LINK_TIMEOUT_MS)
    {
      Motor_SetAll(MOTOR_PULSE_STOP);
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
  * @brief TIM2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM2_Init(void)
{

  /* USER CODE BEGIN TIM2_Init 0 */

  /* USER CODE END TIM2_Init 0 */

  TIM_MasterConfigTypeDef sMasterConfig = {0};
  TIM_OC_InitTypeDef sConfigOC = {0};

  /* USER CODE BEGIN TIM2_Init 1 */

  /* USER CODE END TIM2_Init 1 */
  htim2.Instance = TIM2;
  htim2.Init.Prescaler = 71;
  htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim2.Init.Period = 19999;
  htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_PWM_Init(&htim2) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim2, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sConfigOC.OCMode = TIM_OCMODE_PWM1;
  sConfigOC.Pulse = 1500;
  sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
  sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
  if (HAL_TIM_PWM_ConfigChannel(&htim2, &sConfigOC, TIM_CHANNEL_1) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_TIM_PWM_ConfigChannel(&htim2, &sConfigOC, TIM_CHANNEL_2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM2_Init 2 */

  /* USER CODE END TIM2_Init 2 */
  HAL_TIM_MspPostInit(&htim2);

}

/**
  * @brief TIM4 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM4_Init(void)
{

  /* USER CODE BEGIN TIM4_Init 0 */

  /* USER CODE END TIM4_Init 0 */

  TIM_MasterConfigTypeDef sMasterConfig = {0};
  TIM_OC_InitTypeDef sConfigOC = {0};

  /* USER CODE BEGIN TIM4_Init 1 */

  /* USER CODE END TIM4_Init 1 */
  htim4.Instance = TIM4;
  htim4.Init.Prescaler = 71;
  htim4.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim4.Init.Period = 19999;
  htim4.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim4.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_PWM_Init(&htim4) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim4, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sConfigOC.OCMode = TIM_OCMODE_PWM1;
  sConfigOC.Pulse = 1500;
  sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
  sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
  if (HAL_TIM_PWM_ConfigChannel(&htim4, &sConfigOC, TIM_CHANNEL_3) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_TIM_PWM_ConfigChannel(&htim4, &sConfigOC, TIM_CHANNEL_4) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM4_Init 2 */

  /* USER CODE END TIM4_Init 2 */
  HAL_TIM_MspPostInit(&htim4);

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
  __HAL_RCC_GPIOB_CLK_ENABLE();

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
