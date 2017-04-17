<?php

/**
 * @file
 * Implementation of classes from the batch ingest framework for finding aids.
 */

/**
 * Batch preprocessor.
 */
class IslandoraFindingAidBatch extends IslandoraScanBatch {
  /**
   * Static function to get object class.
   */
  protected static function getObjectClass() {
    return 'IslandoraFindingAidFindingAidBatchObject';
  }

  /**
   * Function preprocess.
   */
  public function preprocess() {
    $added = parent::preprocess();

    $object_type = static::getObjectClass();

    foreach ($added as $object) {
      if ($object instanceof $object_type) {
        db_update('islandora_batch_state')
          ->fields(array(
            'state' => ISLANDORA_BATCH_STATE__READY,
          ))
          ->condition('id', $object->id)
          ->execute();
      }
    }

    return $added;
  }
}

/**
 * Each directory is an object. Each contained file is a datastream.
 *
 * Files are used "DSID.ext", and the extension is used to figure out the
 * mimetype for the datastream.
 * @todo Should be moved into islandora_batch.
 */
class IslandoraFlatBatchObject extends IslandoraScanBatchObject {
  /**
   * Function batchProcess.
   */
  public function batchProcess() {
    // Use object_info to create some datastreams.
    $this->label = $this->getTitle();
    $this->getMods();
    $this->getDc();

    // Add other assets...
    // XXX: This should be made into a constant or sommat?
    // Skip MODS and DC (and --METADATA--), 'cause they get handled in other
    // functions.
    $skip = drupal_map_assoc(array('MODS', 'DC', '--METADATA--'));

    foreach (array_diff_key($this->objectInfo['.'], $skip) as $dsid => $obj) {
      $obj_datastream = $this->constructDatastream($dsid);
      // Figure out the mimetype.
      $obj_datastream->mimetype = static::getMime($obj);
      $obj_datastream->label = "$dsid Datastream";
      $obj_datastream->setContentFromFile($obj->uri, FALSE);

      $this->ingestDatastream($obj_datastream);
    }

    return ISLANDORA_BATCH_STATE__DONE;
  }

  /**
   * Function getMods.
   */
  protected function getMods() {
    if (!isset($this['MODS'])) {
      $mods_datastream = $this->constructDatastream('MODS', 'X');
      $mods_datastream->mimetype = 'text/xml';
      $mods_datastream->label = 'MODS Record';

      $dir = drupal_get_path('module', 'islandora_batch');

      if (isset($this->objectInfo['.']['MODS'])) {
        $mods_datastream->setContentFromFile($this->objectInfo['.']['MODS']->uri, FALSE);
      }
      elseif (isset($this->objectInfo['.']['--METADATA--'])) {
        // Detect type of contents, and transform.
        $this->getModsFromGeneralSource($mods_datastream);
      }
      elseif (isset($this->objectInfo['.']['DC'])) {
        $mods_datastream->content = static::runXslTransform(array(
          'input' => file_get_contents($this->objectInfo['.']['DC']->uri),
          'xsl' => $dir . '/transforms/simpleDC2MODS.xsl',
        ));
      }

      if (!isset($mods_datastream->content)) {
        // If we do not yet have any XML, create a dumb MODS with just the
        // title, and set it...
        $title = $this->baseName;

        $mods_datastream->content = <<<EOXML
<mods xmlns:mods="http://www.loc.gov/mods/v3" xmlns="http://www.loc.gov/mods/v3">
  <titleInfo>
    <title>$title</title>
  </titleInfo>
</mods>
EOXML;
      }

      $this->ingestDatastream($mods_datastream);
    }

    return $this['MODS']->content;
  }

  /**
   * Transform from --METADATA--.{mrc,xml} to MODS.
   *
   * @param AbstractDatastream $datastream
   *   The datastream
   */
  protected function getModsFromGeneralSource(AbstractDatastream $datastream) {
    $meta = $this->objectInfo['.']['--METADATA--'];
    $ext = strtolower(pathinfo($meta->filename, PATHINFO_EXTENSION));
    $dir = drupal_get_path('module', 'islandora_batch');
    if ($ext == 'xml') {
      $xml = file_get_contents($meta->uri);

      if ($xml) {
        $s_xml = new SimpleXMLElement($xml);
        // MODS, set.
        if ($s_xml->getName() == 'mods') {
          // XXX: Should probably create a resource entry.
          $datastream->setContentFromFile($meta->uri, FALSE);
        }
        // MARCXML, transform to MODS and set.
        elseif ($s_xml->getName() == 'record') {
          $datastream->content = static::runXslTransform(array(
            'input' => $xml,
            'xsl' => $dir . '/transforms/MARC21slim2MODS3-4.xsl',
          ));
        }
        // DC, transform to MODS and set.
        elseif ($s_xml->getName() == 'dc') {
          $datastream->content = static::runXslTransform(array(
            'input' => $xml,
            'xsl' => $dir . '/transforms/simpleDC2MODS.xsl',
          ));
        }
        // Unrecognized format...
        else {
          // Do nothing?
        }
      }
    }
    elseif ($ext == 'mrc') {
      // Generate some MODS from binary MARC.
      // Get the java executable.
      $java = variable_get('islandora_batch_java', 'java');

      $temp_file_marc = tempnam('/tmp', 'binmrc');
      $temp_file_mods = tempnam('/tmp', 'mods');

      // Copy the file, so we're sure it's accessible on the filesystem.
      copy($meta->uri, $temp_file_marc);
      $absolute = realpath($dir);

      // The command to run.
      $command = "$java -cp $absolute/transforms/marcxml/marcxml.jar:$absolute/transforms/marcxml/marc4j.jar gov.loc.marcxml.MARC2MODS $temp_file_marc $temp_file_mods";

      // Run the command.
      $proc = popen($command, 'r');
      pclose($proc);

      // Load the output into a DOMDocument.
      $doc = new DOMDocument();
      $doc->load($temp_file_mods);

      // Drop the temp.
      unlink($temp_file_marc);
      unlink($temp_file_mods);

      // The transformation produces a collection of mods docs (even if there's
      // only one)...  Let's grab the first one.
      $mods_docs = $doc->getElementsByTagName('mods');

      if ($mods_docs->length > 0) {
        // Grab the first.
        $mods = new DOMDocument();
        $mods_node = $mods_docs->item(0);
        $mods_node = $mods->importNode($mods_node, TRUE);
        $mods->appendChild($mods_node);

        // TODO: Transform dates to Solr format...  Or should this be left to
        // a core hook?
        // And put the content into the datastream.
        $datastream->content = $mods->saveXML();
      }
    }
  }
  /**
   * Function batchProcess.
   */
  protected function getDc() {
    if (!isset($this['DC'])) {
      // Create the DC datastream on this object.
      $dc_datastream = $this->constructDatastream('DC', 'X');
      $dc_datastream->mimetype = 'text/xml';
      $dc_datastream->label = 'DC Record';

      if (isset($this->objectInfo['.']['DC'])) {
        $dc_datastream->setContentFromFile($this->objectInfo['.']['DC']->uri, FALSE);
      }
      elseif (isset($this->objectInfo['.']['--METADATA--'])) {
        // If --METADATA-- contains DC directly, use it.
        $this->getDcFromGeneralSource($dc_datastream);
      }

      // Get the DC by transforming from MODS.
      if (!isset($dc_datastream->content)) {
        // XXX: Might want to make this use a file, instead of staging the DC
        // in the database table (inside the object we serialize).
        $new_dc = static::runXslTransform(array(
          'xsl' => drupal_get_path('module', 'islandora_batch') . '/transforms/mods_to_dc.xsl',
          'input' => $this->getMods(),
        ));
        if ($new_dc) {
          $dc_datastream->content = $new_dc;
        }
      }

      if (isset($dc_datastream->content)) {
        $this->ingestDatastream($dc_datastream);
      }
    }

    return $this['DC']->content;
  }

  /**
   * If --METADATA--.xml happens to be DC, grab it...
   *
   * ... otherwise, we'll let the MODS implementation transform it.
   *
   * @param AbstractDatastream $datastream
   *   The datastream to which we should add the metadata.
   */
  protected function getDcFromGeneralSource(AbstractDatastream $datastream) {
    $meta = $this->objectInfo['.']['--METADATA--'];

    if ('xml' == pathinfo($meta->filename, PATHINFO_EXTENSION)) {
      $xml = file_get_contents($meta->uri);

      if ($xml) {
        $s_xml = new SimpleXMLElement($xml);
        if ($s_xml->getName() == 'dc') {
          // XXX: Should probably create a resource entry.
          $datastream->setContentFromFile($meta->uri, FALSE);
        }
      }
    }
  }
}

/**
 * Class IslandoraFindingAidBatchObject.
 */
class IslandoraFindingAidFindingAidBatchObject extends IslandoraFlatBatchObject {
  /**
   * Function class constructor.
   */
  public function __construct(IslandoraTuque $connection, $base_name, $object_info, $pp) {
    parent::__construct($connection, $base_name, $object_info, $pp);
  }
  
  /**
   * Function initialBatchState.
   */
  public function initialBatchState() {
    // Not ready until we've added the pages.
    return ISLANDORA_BATCH_STATE__NOT_READY;
  }

  /**
   * Function addRelationships.
   */
  public function addRelationships() {
    parent::addRelationships();

    // Identify the source of this object.
    $this->relationships->add(ISLANDORA_RELS_EXT_URI, 'findingaids-batched', 'true', RELS_TYPE_PLAIN_LITERAL);
    if (isset($this->preprocessorParameters['email_admin']) && $this->preprocessorParameters['email_admin']) {
      $this->relationships->add(ISLANDORA_RELS_EXT_URI, 'email-admin-when-ingested', 'true', RELS_TYPE_PLAIN_LITERAL);
    }
  }
}
