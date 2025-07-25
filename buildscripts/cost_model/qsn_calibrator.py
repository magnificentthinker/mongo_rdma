# Copyright (C) 2022-present MongoDB, Inc.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the Server Side Public License, version 1,
# as published by MongoDB, Inc.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# Server Side Public License for more details.
#
# You should have received a copy of the Server Side Public License
# along with this program. If not, see
# <http://www.mongodb.com/licensing/server-side-public-license>.
#
# As a special exception, the copyright holders give permission to link the
# code of portions of this program with the OpenSSL library under certain
# conditions as described in each individual source file and distribute
# linked combinations including the program with the OpenSSL library. You
# must comply with the Server Side Public License in all respects for
# all of the code used other than as permitted herein. If you modify file(s)
# with this exception, you may extend this exception to your version of the
# file(s), but you are not obligated to do so. If you do not wish to do so,
# delete this exception statement from your version. If you delete this
# exception statement from all source files in the program, then also delete
# it in the license file.
#
"""Calibrate QSN nodes."""

from __future__ import annotations

import experiment as exp
import matplotlib.pyplot as plt
import pandas as pd
import statsmodels.api as sm
from config import QsNodeCalibrationConfig, QuerySolutionCalibrationConfig
from cost_estimator import estimate
from database_instance import DatabaseInstance
from sklearn.linear_model import LinearRegression

__all__ = ["calibrate"]


async def calibrate(config: QuerySolutionCalibrationConfig, database: DatabaseInstance):
    """Main entry-point for QSN calibration."""

    if not config.enabled:
        return {}

    df = await exp.load_calibration_data(database, config.input_collection_name)
    noout_df = exp.remove_outliers(df, 0.0, 0.90)
    qsn_df = exp.extract_qsn_nodes(noout_df)
    result = {}
    for node_config in config.nodes:
        result[node_config.type] = calibrate_node(qsn_df, config, node_config)
    return result


def calibrate_node(
    qsn_df: pd.DataFrame,
    config: QuerySolutionCalibrationConfig,
    node_config: QsNodeCalibrationConfig,
):
    qsn_node_df = qsn_df[qsn_df.node_type == node_config.type]
    if node_config.filter_function is not None:
        qsn_node_df = node_config.filter_function(qsn_node_df)

    if node_config.variables_override is None:
        variables = ["n_processed"]
    else:
        variables = node_config.variables_override
    y = qsn_node_df["execution_time"]
    X = qsn_node_df[variables]

    X = sm.add_constant(X)

    def fit(X, y):
        nnls = LinearRegression(positive=True, fit_intercept=False)
        model = nnls.fit(X, y)
        return (model.coef_, model.predict)

    model = estimate(fit, X.to_numpy(), y.to_numpy(), config.test_size, config.trace)
    # plot regression and save to file
    if model.predict:
        fig, ax = plt.subplots()
        ax.scatter(qsn_node_df[variables], y, label="Executions")
        ax.plot(
            qsn_node_df[variables],
            model.predict(X),
            linewidth=3,
            color="tab:orange",
            label="Linear Regression",
        )
        ax.set(
            xlabel="Number of documents",
            ylabel="Execution time (ns)",
            title=f"Regression for {node_config.type}",
        )
        ax.legend()
        fig.savefig(f"{node_config.type}.png")
    return model
