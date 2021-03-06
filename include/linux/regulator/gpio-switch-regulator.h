#ifndef _GPIO_SWITCH_REGULATOR_H
#define _GPIO_SWITCH_REGULATOR_H

#include <linux/regulator/machine.h>

/*
 * struct gpio_switch_regulator_subdev_data - Gpio switch regulator subdevice
 * data.
 *
 * Subdevice data to register a gpio regulator switch device driver.
 *
 * @regulator_name: The name of regulator.
 * @input_supply: Input supply name.
 * @id: The id of the switch.
 * @gpio_nr: Gpio nr which controls this switch.
 * @active_low: true if making gpio low makes voltage output enable.
 * @init_state: 1 if init_state should be active.
 * @voltages: Possible voltages to set at output. The values are in millivolt.
 * @n_voltages: Number of voltages.
 * @num_consumer_supplies: Number of cosumer supplies.
 * @consumer_supplies: List of consumer spllies.
 */
struct gpio_switch_regulator_subdev_data {
	const char	*regulator_name;
	const char	*input_supply;
	int id;
	int gpio_nr;
	int active_low;
	int pin_group;
	int init_state;
	int *voltages;
	unsigned n_voltages;
	struct regulator_consumer_supply *consumer_supplies;
	int num_consumer_supplies;
	struct regulation_constraints constraints;
	int (*enable_rail)(struct gpio_switch_regulator_subdev_data *pdata);
	int (*disable_rail)(struct gpio_switch_regulator_subdev_data *pdata);

};

/**
 * gpio_switch_regulator_platform_data - platform data for gpio_switch_regulator
 * @num_subdevs: number of regulators used
 * @subdevs: pointer to regulators used
 */
struct gpio_switch_regulator_platform_data {
	int num_subdevs;
	struct gpio_switch_regulator_subdev_data **subdevs;
};

#endif
