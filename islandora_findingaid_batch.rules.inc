<?php

/**
 * @file
 * Rules hook implementations.
 */

/**
 * Implements hook_rules_event_info().
 */
function islandora_findingaid_batch_rules_event_info() {
  $events = array();

  $events['islandora_findingaid_batch_ingested'] = array(
    'group' => t('Islandora Finding Aids'),
    'label' => t('Finding Aids ingested'),
    'variables' => array(
      'finding_aid_object' => array(
        'type' => 'islandora_object',
        'label' => t('The ingested finding aid'),
        'description' => t('A Tuque object for the ingested finding aid, as an entity.'),
      ),
    ),
  );

  return $events;
}
