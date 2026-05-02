/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
* Copyright 2026 Eduard Siboshvili <edsiboshvili@gmail.com>
* Use is subject to license terms.
*/



#include "re.h"


static int
re_sensor_read_temperature(void *arg, sensor_ioctl_scalar_t *scalar)
{
	struct re_softc *sc = arg;
	scalar->sis_unit = SENSOR_UNIT_CELSIUS;
	scalar->sis_gran = 1;
	scalar->sis_prec = 0;
	scalar->sis_value = re_read_thermal_sensor(sc);
	return 0;
}

static const ksensor_ops_t re_temp_ops = {
	.kso_kind = ksensor_kind_temperature,
	.kso_scalar = re_sensor_read_temperature
};

void
re_sensor_init(struct re_softc *sc)
{
	int err;
	char name[32];
	if(sc->re_type == MAC_R25) {
		return;
	}

	(void)snprintf(name, sizeof (name), "%s%d_ethernet",
			RE_MOD_NAME,
			ddi_get_instance(sc->dev));

	if ((err = ksensor_create_scalar_pcidev(sc->dev,
		SENSOR_KIND_TEMPERATURE, &re_temp_ops, sc, name,
		&sc->sensor.isn_reg_ksensor)) !=
		0) {
		dev_err(sc->dev, CE_WARN, "failed to create ksensor, err: %d", err);
	}

	sc->sensor.isn_valid = B_TRUE;
}


void
re_sensor_fini(struct re_softc *sc)
{
    if(sc->sensor.isn_valid)
	    (void) ksensor_remove(sc->dev, KSENSOR_ALL_IDS);
    sc->sensor.isn_valid = B_FALSE;
}
