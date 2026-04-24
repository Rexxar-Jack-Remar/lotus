/**
 * @file Config.h
 * @brief Configuration utilities for FPA (Function Pointer Analysis)
 *
 * This file provides configuration settings for the FPA analysis framework,
 * including debug mode control and maximum type layer settings used during
 * type-based function pointer analysis.
 *
 * @ingroup FPA
 */

//
// Created by prophe cheng on 2024/1/3.
//

#ifndef TYPEDIVE_CONFIG_H
#define TYPEDIVE_CONFIG_H

// 采用signature match进行类型匹配或者逐个匹配类型
extern bool debug_mode;
extern int max_type_layer;

#endif // TYPEDIVE_CONFIG_H