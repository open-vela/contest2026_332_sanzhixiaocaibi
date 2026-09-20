/****************************************************************************
 * boards/risc-v/esp32p4/esp32p4-function-ev-board/src/esp32p4_guardd.c
 *
 * 电脉卫士（Electric Guard P4）—— openvela 侧执行守护进程
 *
 * 作为系统 init 入口常驻运行（CONFIG_INIT_ENTRYPOINT=guardd_main），
 * 从 /dev/console 读单字符指令，驱动配电柜的三相断闸固态继电器与蜂鸣器，
 * 每次动作回报一行结果。
 *
 * 指令由 PC 侧网关 tools/guard_bridge.py 下发，网关负责把电气故障事件与
 * 视觉火情置信度交给大模型研判。判断端在网关，执行端在 openvela ——
 * 但断闸是安全动作，不能把执行的确定性交给一条可能超时的网络链路，所以
 * 这里收到 'c' 立刻断，不等二次确认；网关掉线也不影响已下发的动作。
 *
 * 为什么放在板级代码而不是 apps/examples：在本移植树上新增 builtin 应用
 * 会导致 nsh_consolemain() 启动时挂死（见 evidence/ 下的记录），根因尚未
 * 定位。放在板级并直接作为 init 入口可完全绕开 apps/builtin 那条链路。
 *
 * 协议（单字符；输出行以 "GUARD " 开头便于网关解析）：
 *   c  cut      断闸：GPIO21 拉高，蜂鸣器长鸣
 *   r  restore  复位：GPIO21 拉低
 *   b  beep     仅鸣笛，不断闸（对应 Lv2 告警）
 *   s  status   回报当前引脚状态
 *   h  help     指令表
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>

#include <nuttx/ioexpander/gpio.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define GUARD_DEV_CUT    "/dev/gpio0"   /* GPIO21 -> 光耦 -> SSR-40DA x3 */
#define GUARD_DEV_BUZZER "/dev/gpio1"   /* GPIO22 -> 蜂鸣器              */
#define GUARD_DEV_CONS   "/dev/console"

#define BEEP_ON_MS       120
#define BEEP_OFF_MS      100
#define BEEP_COUNT_ALARM 3
#define BEEP_COUNT_CUT   6

/****************************************************************************
 * Private Data
 ****************************************************************************/

static int g_fd_cut    = -1;
static int g_fd_buzzer = -1;
/* 不再自己 open("/dev/console")。init 任务的 fd 0/1 由内核在 nx_start 阶段
 * 建好，再 open 一次会走 uart_open->esp_setup，实测在冷上电后会永久阻塞。
 */

static int g_fd_out    = 1;   /* 继承自 init 任务的 stdout */
static int g_fd_in     = 0;   /* 继承自 init 任务的 stdin  */

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void guard_say(FAR const char *line)
{
  extern void ets_printf(const char *fmt, ...);

  if (g_fd_out < 0 || write(g_fd_out, line, strlen(line)) < 0)
    {
      /* stdio 不可用时退回 ROM 打印，保证演示始终有输出 */

      ets_printf("%s", line);
    }
}

static int guard_pin_write(int fd, bool value)
{
  if (fd < 0)
    {
      return -ENODEV;
    }

  return ioctl(fd, GPIOC_WRITE, (unsigned long)value);
}

static int guard_pin_read(int fd, FAR bool *value)
{
  if (fd < 0)
    {
      return -ENODEV;
    }

  return ioctl(fd, GPIOC_READ, (unsigned long)value);
}

static void guard_beep(int times)
{
  int i;

  for (i = 0; i < times; i++)
    {
      guard_pin_write(g_fd_buzzer, true);
      usleep(BEEP_ON_MS * 1000);
      guard_pin_write(g_fd_buzzer, false);
      usleep(BEEP_OFF_MS * 1000);
    }
}

static void guard_status(void)
{
  char buf[64];
  bool cut    = false;
  bool buzzer = false;

  guard_pin_read(g_fd_cut, &cut);
  guard_pin_read(g_fd_buzzer, &buzzer);

  snprintf(buf, sizeof(buf), "GUARD STATUS cut=%d buzzer=%d\n",
           (int)cut, (int)buzzer);
  guard_say(buf);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int guardd_main(int argc, FAR char *argv[])
{
  char buf[64];
  char ch;
  int  ret;
  extern void ets_printf(const char *fmt, ...);

  ets_printf("GD:0 guardd-enter%c", 10);

  g_fd_cut = open(GUARD_DEV_CUT, O_RDWR);
  if (g_fd_cut < 0)
    {
      snprintf(buf, sizeof(buf), "GUARD ERROR open %s errno=%d\n",
               GUARD_DEV_CUT, errno);
      guard_say(buf);
    }

  g_fd_buzzer = open(GUARD_DEV_BUZZER, O_RDWR);
  if (g_fd_buzzer < 0)
    {
      snprintf(buf, sizeof(buf), "GUARD ERROR open %s errno=%d\n",
               GUARD_DEV_BUZZER, errno);
      guard_say(buf);
    }

  /* 上电保证继电器不吸合，避免一通电就把配电柜断掉 */

  guard_pin_write(g_fd_cut, false);
  guard_pin_write(g_fd_buzzer, false);

  guard_say("GUARD READY cut=/dev/gpio0(GPIO21) buzzer=/dev/gpio1(GPIO22)\n");
  guard_say("GUARD HELP c=cut r=restore b=beep s=status\n");

  for (; ; )
    {
      ret = read(g_fd_in, &ch, 1);
      if (ret <= 0)
        {
          if (ret < 0 && errno == EINTR)
            {
              continue;
            }

          usleep(50 * 1000);
          continue;
        }

      switch (ch)
        {
          case 'c':
          case 'C':
            {
              ret = guard_pin_write(g_fd_cut, true);
              snprintf(buf, sizeof(buf), "GUARD CUT ret=%d\n", ret);
              guard_say(buf);
              guard_beep(BEEP_COUNT_CUT);
            }
            break;

          case 'r':
          case 'R':
            {
              ret = guard_pin_write(g_fd_cut, false);
              snprintf(buf, sizeof(buf), "GUARD RESTORE ret=%d\n", ret);
              guard_say(buf);
            }
            break;

          case 'b':
          case 'B':
            {
              guard_say("GUARD BEEP\n");
              guard_beep(BEEP_COUNT_ALARM);
            }
            break;

          case 's':
          case 'S':
            guard_status();
            break;

          case 'h':
          case 'H':
            guard_say("GUARD HELP c=cut r=restore b=beep s=status\n");
            break;

          default:

            /* 回车、换行与其它噪声字符直接忽略 */

            break;
        }
    }

  return 0;
}
