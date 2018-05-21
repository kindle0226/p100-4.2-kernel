#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/ratelimit.h>

#include "common.h"

#include "voltage.h"
#include "vp.h"
#include "prm-regbits-34xx.h"
#include "prm-regbits-44xx.h"
#include "prm44xx.h"

/**
 * omap_vp_get_curr_volt() - API to get the current vp voltage.
 * @voltdm:	pointer to the VDD.
 *
 * This API returns the current voltage for the specified voltage processor
 */
unsigned long omap_vp_get_curr_volt(struct voltagedomain *voltdm)
{
	struct omap_vp_instance *vp;
	u8 curr_vsel;

	if (IS_ERR_OR_NULL(voltdm)) {
		pr_warning("%s: VDD specified does not exist!\n", __func__);
		return 0;
	}

	if (!voltdm->read) {
		pr_err("%s: No read API for reading vdd_%s regs\n",
		       __func__, voltdm->name);
		return 0;
	}

	vp = voltdm->vp;
	if (IS_ERR_OR_NULL(vp)) {
		pr_err("%s: No VP info for vdd_%s\n", __func__, voltdm->name);
		return 0;
	}

	curr_vsel = (voltdm->read(vp->voltage) & vp->common->vpvoltage_mask)
		>> __ffs(vp->common->vpvoltage_mask);

	if (!voltdm->pmic || !voltdm->pmic->vsel_to_uv) {
		pr_warning("%s: PMIC function vsel_to_uv not registered\n",
			   __func__);
		return 0;
	}

	return voltdm->pmic->vsel_to_uv(curr_vsel);
}

/**
 * _vp_wait_for_idle() - wait for voltage processor to idle
 * @voltdm:	voltage domain
 * @vp:		voltage processor instance
 *
 * In some conditions, it is important to ensure that Voltage Processor
 * is idle before performing operations on the Voltage Processor(VP).
 * This is primarily to ensure that VP state machine does not enter into
 * invalid state.
 *
 * Returns -ETIMEDOUT if timeout occurs - This could be critical failure
 * as it indicates that Voltage processor might have it's state machine
 * stuck up without recovering out(theoretically should never happen
 * ofcourse). Returns 0 if idle state is detected.
 *
 * Note: callers are expected to ensure requisite checks are performed
 * on the pointers passed.
 */
static inline int _vp_wait_for_idle(struct voltagedomain *voltdm,
				    struct omap_vp_instance *vp)
{
	int timeout;

	omap_test_timeout((voltdm->read(vp->vstatus) &
			   vp->common->vstatus_vpidle), VP_IDLE_TIMEOUT,
			  timeout);

	if (timeout >= VP_IDLE_TIMEOUT) {
		/* Dont spam the console but ensure we catch attention */
		pr_warn_ratelimited("%s: vdd_%s idle timedout\n",
				    __func__, voltdm->name);
		WARN_ONCE("vdd_%s idle timedout\n", voltdm->name);

		return -ETIMEDOUT;
	}

	return 0;
}

static u32 _vp_set_init_voltage(struct voltagedomain *voltdm,
				unsigned long volt)
{
	struct omap_vp_instance *vp = voltdm->vp;
	u32 vpconfig;
	char vsel;

	vsel = voltdm->pmic->uv_to_vsel(volt);

	vpconfig = voltdm->read(vp->vpconfig);
	vpconfig &= ~(vp->common->vpconfig_initvoltage_mask |
		      vp->common->vpconfig_forceupdate |
		      vp->common->vpconfig_initvdd);
	vpconfig |= vsel << __ffs(vp->common->vpconfig_initvoltage_mask);
	voltdm->write(vpconfig, vp->vpconfig);

	/* Trigger initVDD value copy to voltage processor */
	voltdm->write((vpconfig | vp->common->vpconfig_initvdd),
		       vp->vpconfig);

	/* Clear initVDD copy trigger bit */
	voltdm->write(vpconfig, vp->vpconfig);

	return vpconfig;
}

/* Generic voltage init functions */
void __init omap_vp_init(struct voltagedomain *voltdm)
{
	struct omap_vp_instance *vp;
	u32 val, sys_clk_rate, timeout, waittime;
	u32 vddmin, vddmax, vstepmin, vstepmax;

	if (IS_ERR_OR_NULL(voltdm)) {
		pr_err("%s: VDD specified does not exist!\n", __func__);
		return;
	}

	if (!voltdm->pmic || !voltdm->pmic->uv_to_vsel) {
		pr_err("%s: No PMIC info for vdd_%s\n", __func__, voltdm->name);
		return;
	}

	if (!voltdm->read || !voltdm->write) {
		pr_err("%s: No read/write API for accessing vdd_%s regs\n",
			__func__, voltdm->name);
		return;
	}

	vp = voltdm->vp;
	if (IS_ERR_OR_NULL(vp)) {
		pr_err("%s: No VP info for vdd_%s\n", __func__, voltdm->name);
		return;
	}

	if (IS_ERR_OR_NULL(voltdm->vc_param)) {
		pr_err("%s: No vc_param info for vdd_%s\n",
		       __func__, voltdm->name);
		return;
	}

	if (IS_ERR_OR_NULL(voltdm->vp_param)) {
		pr_err("%s: No vp_param info for vdd_%s\n",
		       __func__, voltdm->name);
		return;
	}

	vp->enabled = false;

	/* Divide to avoid overflow */
	sys_clk_rate = voltdm->sys_clk.rate / 1000;

	timeout = (sys_clk_rate * voltdm->pmic->vp_timeout_us) / 1000;
	vddmin = max(voltdm->vp_param->vddmin, voltdm->pmic->vddmin);
	vddmin = max(vddmin, voltdm->vc_param->ret);
	vddmax = min(voltdm->vp_param->vddmax, voltdm->pmic->vddmax);
	vddmin = voltdm->pmic->uv_to_vsel(vddmin);
	vddmax = voltdm->pmic->uv_to_vsel(vddmax);

	waittime = DIV_ROUND_UP(voltdm->pmic->step_size * sys_clk_rate,
				1000 * voltdm->pmic->slew_rate);
	vstepmin = voltdm->pmic->vp_vstepmin;
	vstepmax = voltdm->pmic->vp_vstepmax;

	/*
	 * VP_CONFIG: error gain is not set here, it will be updated
	 * on each scale, based on OPP.
	 */
	val = (voltdm->pmic->vp_erroroffset <<
	       __ffs(voltdm->vp->common->vpconfig_erroroffset_mask)) |
		vp->common->vpconfig_timeouten;
	voltdm->write(val, vp->vpconfig);

	/* VSTEPMIN */
	val = (waittime << vp->common->vstepmin_smpswaittimemin_shift) |
		(vstepmin <<  vp->common->vstepmin_stepmin_shift);
	voltdm->write(val, vp->vstepmin);

	/* VSTEPMAX */
	val = (vstepmax << vp->common->vstepmax_stepmax_shift) |
		(waittime << vp->common->vstepmax_smpswaittimemax_shift);
	voltdm->write(val, vp->vstepmax);

	/* VLIMITTO */
	val = (vddmax << vp->common->vlimitto_vddmax_shift) |
		(vddmin << vp->common->vlimitto_vddmin_shift) |
		(timeout <<  vp->common->vlimitto_timeout_shift);
	voltdm->write(val, vp->vlimitto);
}

/**
 * omap_vp_is_transdone() - is voltage transfer done on vp?
 * @voltdm:	pointer to the VDD which is to be scaled.
 *
 * VP's transdone bit is the only way to ensure that the transfer
 * of the voltage value has actually been send over to the PMIC
 * This is hence useful for all users of voltage domain to precisely
 * identify once the PMIC voltage has been set by the voltage processor
 */
bool omap_vp_is_transdone(struct voltagedomain *voltdm)
{

	struct omap_vp_instance *vp = voltdm->vp;

	return vp->common->ops->check_txdone(vp->id) ? true : false;
}

/**
 * omap_vp_clear_transdone() - clear voltage transfer done status on vp
 * @voltdm:	pointer to the VDD which is to be scaled.
 */
void omap_vp_clear_transdone(struct voltagedomain *voltdm)
{
	struct omap_vp_instance *vp = voltdm->vp;

	vp->common->ops->clear_txdone(vp->id);

	return;
}

int omap_vp_update_errorgain(struct voltagedomain *voltdm,
			     struct omap_volt_data *volt_data)
{
	if (IS_ERR_OR_NULL(voltdm)) {
		pr_err("%s: VDD specified does not exist!\n", __func__);
		return 0;
	}

	if (!voltdm->vp)
		return -EINVAL;

	if (IS_ERR_OR_NULL(volt_data)) {
		pr_err("%s: vdm %s no voltage data for %p\n", __func__,
		       voltdm->name, volt_data);
		return -EINVAL;
	}

	if (IS_ERR_OR_NULL(voltdm->rmw)) {
		pr_err("%s: No rmw API for reading vdd_%s regs\n",
		       __func__, voltdm->name);
		return 0;
	}

	/* Setting vp errorgain based on the voltage */
	voltdm->rmw(voltdm->vp->common->vpconfig_errorgain_mask,
		    volt_data->vp_errgain <<
		    __ffs(voltdm->vp->common->vpconfig_errorgain_mask),
		    voltdm->vp->vpconfig);

	return 0;
}

#define _MAX_RETRIES_BEFORE_RECOVER 50
#define _MAX_COUNT_ERR		10
static u8 __vp_debug_error_message_count = _MAX_COUNT_ERR;
static u8 __vp_recover_count = _MAX_RETRIES_BEFORE_RECOVER;
/* Dump with stack the first few messages, tone down severity for the rest */
#define _vp_controlled_err(vp, voltdm, ARGS...)				\
do {									\
	if (__vp_debug_error_message_count) {				\
		pr_err(ARGS);						\
		dump_stack();						\
		__vp_debug_error_message_count--;			\
	} else {							\
		do {							\
			pr_err_ratelimited(ARGS);			\
		} while (0);						\
	}								\
	if ((vp)->common->ops->recover && !(--__vp_recover_count)) {	\
		pr_err("%s:domain %s recovery count triggered\n",	\
			__func__, (voltdm)->name);			\
		(vp)->common->ops->recover((vp)->id);			\
		__vp_recover_count = _MAX_RETRIES_BEFORE_RECOVER;	\
	}								\
} while (0)


/* VP force update method of voltage scaling */
int omap_vp_forceupdate_scale(struct voltagedomain *voltdm,
			      struct omap_volt_data *target_v)
{
	struct omap_vp_instance *vp;
	u32 vpconfig;
	u8 target_vsel, current_vsel;
	int ret, timeout = 0;
	unsigned long target_volt;

	if (IS_ERR_OR_NULL(voltdm)) {
		pr_err("%s: VDD specified does not exist!\n", __func__);
		return -EINVAL;
	}

	if (IS_ERR_OR_NULL(voltdm->write)) {
		pr_err("%s: No write API for writing vdd_%s regs\n",
		       __func__, voltdm->name);
		return -EINVAL;
	}

	if (IS_ERR_OR_NULL(target_v)) {
		pr_err("%s: No target_v info to scale vdd_%s\n",
		       __func__, voltdm->name);
		return -EINVAL;
	}

	vp = voltdm->vp;
	if (IS_ERR_OR_NULL(vp)) {
		pr_err("%s: No VP info for vdd_%s\n", __func__, voltdm->name);
		return -EINVAL;
	}

	target_volt = omap_get_operation_voltage(target_v);

	ret = _vp_wait_for_idle(voltdm, vp);
	if (ret) {
		_vp_controlled_err(vp, voltdm,
				   "%s: vdd_%s idle timedout (v=%ld)\n",
				   __func__, voltdm->name, target_volt);
		return ret;
	}

	ret = omap_vc_pre_scale(voltdm, target_volt, target_v,
				&target_vsel, &current_vsel);
	if (ret)
		return ret;

	/*
	 * Clear all pending TransactionDone interrupt/status. Typical latency
	 * is <3us
	 */
	while (timeout++ < VP_TRANXDONE_TIMEOUT) {
		vp->common->ops->clear_txdone(vp->id);
		if (!vp->common->ops->check_txdone(vp->id))
			break;
		udelay(1);
	}
	if (timeout >= VP_TRANXDONE_TIMEOUT) {
		_vp_controlled_err(vp, voltdm,
			"%s: vdd_%s TRANXDONE timeout exceeded."
			"Voltage change aborted target volt=%ld,"
			"target vsel=0x%02x, current_vsel=0x%02x\n",
			__func__, voltdm->name, target_volt,
			target_vsel, current_vsel);
		return -ETIMEDOUT;
	}

	vpconfig = _vp_set_init_voltage(voltdm, target_volt);

	/* Force update of voltage */
	voltdm->write(vpconfig | vp->common->vpconfig_forceupdate,
		      voltdm->vp->vpconfig);

	/*
	 * Wait for TransactionDone. Typical latency is <200us.
	 * Depends on SMPSWAITTIMEMIN/MAX and voltage change
	 */
	timeout = 0;
	omap_test_timeout(vp->common->ops->check_txdone(vp->id),
			  VP_TRANXDONE_TIMEOUT, timeout);
	if (timeout >= VP_TRANXDONE_TIMEOUT)
		_vp_controlled_err(vp, voltdm,
			"%s: vdd_%s TRANXDONE timeout exceeded. "
			"TRANXDONE never got set after the voltage update. "
			"target volt=%ld, target vsel=0x%02x, "
			"current_vsel=0x%02x\n",
			__func__, voltdm->name, target_volt,
			target_vsel, current_vsel);

	omap_vc_post_scale(voltdm, target_volt, target_v,
			   target_vsel, current_vsel);

	/*
	 * Disable TransactionDone interrupt , clear all status, clear
	 * control registers
	 */
	timeout = 0;
	while (timeout++ < VP_TRANXDONE_TIMEOUT) {
		vp->common->ops->clear_txdone(vp->id);
		if (!vp->common->ops->check_txdone(vp->id))
			break;
		udelay(1);
	}

	if (timeout >= VP_TRANXDONE_TIMEOUT)
		_vp_controlled_err(vp, voltdm,
			"%s: vdd_%s TRANXDONE timeout exceeded while"
			"trying to clear the TRANXDONE status. target volt=%ld,"
			"target vsel=0x%02x, current_vsel=0x%02x\n",
			__func__, voltdm->name, target_volt,
			target_vsel, current_vsel);

	/* Clear force bit */
	voltdm->write(vpconfig, vp->vpconfig);

	return 0;
}

/**
 * omap_vp_enable() - API to enable a particular VP
 * @voltdm:	pointer to the VDD whose VP is to be enabled.
 *
 * This API enables a particular voltage processor. Needed by the smartreflex
 * class drivers.
 */
void omap_vp_enable(struct voltagedomain *voltdm)
{
	struct omap_vp_instance *vp;
	u32 vpconfig;
	struct omap_volt_data *volt;

	if (IS_ERR_OR_NULL(voltdm)) {
		pr_err("%s: VDD specified does not exist!\n", __func__);
		return;
	}

	vp = voltdm->vp;
	if (IS_ERR_OR_NULL(vp)) {
		pr_err("%s: No VP info for vdd_%s\n", __func__, voltdm->name);
		return;
	}

	if (!voltdm->read || !voltdm->write) {
		pr_err("%s: No read/write API for accessing vdd_%s regs\n",
			__func__, voltdm->name);
		return;
	}

	/* If VP is already enabled, do nothing. Return */
	if (vp->enabled)
		return;

	volt = omap_voltage_get_curr_vdata(voltdm);
	if (!volt) {
		pr_warning("%s: unable to find current voltage for %s\n",
			   __func__, voltdm->name);
		return;
	}

	vpconfig = _vp_set_init_voltage(voltdm,
					omap_get_operation_voltage(volt));

	/* Enable VP */
	vpconfig |= vp->common->vpconfig_vpenable;
	voltdm->write(vpconfig, vp->vpconfig);

	vp->enabled = true;
}

/**
 * omap_vp_disable() - API to disable a particular VP
 * @voltdm:	pointer to the VDD whose VP is to be disabled.
 *
 * This API disables a particular voltage processor. Needed by the smartreflex
 * class drivers.
 */
void omap_vp_disable(struct voltagedomain *voltdm)
{
	struct omap_vp_instance *vp;
	u32 vpconfig;

	if (IS_ERR_OR_NULL(voltdm)) {
		pr_err("%s: VDD specified does not exist!\n", __func__);
		return;
	}

	vp = voltdm->vp;
	if (IS_ERR_OR_NULL(vp)) {
		pr_err("%s: No VP info for vdd_%s\n", __func__, voltdm->name);
		return;
	}

	if (!voltdm->read || !voltdm->write) {
		pr_err("%s: No read/write API for accessing vdd_%s regs\n",
			__func__, voltdm->name);
		return;
	}

	/* If VP is already disabled, do nothing. Return */
	if (!vp->enabled) {
		pr_warning("%s: Trying to disable VP for vdd_%s when"
			"it is already disabled\n", __func__, voltdm->name);
		return;
	}

	if (_vp_wait_for_idle(voltdm, vp)) {
		pr_warn_ratelimited("%s: vdd_%s timedout!Ignore and try\n",
				    __func__, voltdm->name);
	}
	/* Disable VP */
	vpconfig = voltdm->read(vp->vpconfig);
	vpconfig &= ~vp->common->vpconfig_vpenable;
	voltdm->write(vpconfig, vp->vpconfig);

	if (_vp_wait_for_idle(voltdm, vp)) {
		pr_warn_ratelimited("%s: vdd_%s timedout after disable!!\n",
				    __func__, voltdm->name);
	}

	vp->enabled = false;

	return;
}
